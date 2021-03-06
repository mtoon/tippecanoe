#include <iostream>
#include <fstream>
#include <string>
#include <stack>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <zlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <cmath>
#include <sqlite3.h>
#include <pthread.h>
#include <errno.h>
#include "mvt.hpp"
#include "geometry.hpp"
#include "tile.hpp"
#include "pool.hpp"
#include "projection.hpp"
#include "serial.hpp"
#include "options.hpp"
#include "main.hpp"
#include "mbtiles.hpp"

#define CMD_BITS 3

#define XSTRINGIFY(s) STRINGIFY(s)
#define STRINGIFY(s) #s

pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t var_lock = PTHREAD_MUTEX_INITIALIZER;

std::vector<mvt_geometry> to_feature(drawvec &geom) {
	std::vector<mvt_geometry> out;

	for (size_t i = 0; i < geom.size(); i++) {
		out.push_back(mvt_geometry(geom[i].op, geom[i].x, geom[i].y));
	}

	return out;
}

bool draws_something(drawvec &geom) {
	for (size_t i = 1; i < geom.size(); i++) {
		if (geom[i].op == VT_LINETO && (geom[i].x != geom[i - 1].x || geom[i].y != geom[i - 1].y)) {
			return true;
		}
	}

	return false;
}

int metacmp(int m1, const std::vector<long long> &keys1, const std::vector<long long> &values1, char *stringpool1, int m2, const std::vector<long long> &keys2, const std::vector<long long> &values2, char *stringpool2);
int coalindexcmp(const struct coalesce *c1, const struct coalesce *c2);
static int is_integer(const char *s, long long *v);

struct coalesce {
	char *meta;
	char *stringpool;
	std::vector<long long> keys;
	std::vector<long long> values;
	drawvec geom;
	unsigned long long index;
	unsigned long long index2;
	long long original_seq;
	int type;
	int m;
	bool coalesced;
	double spacing;
	bool has_id;
	unsigned long long id;

	bool operator<(const coalesce &o) const {
		int cmp = coalindexcmp(this, &o);
		if (cmp < 0) {
			return true;
		} else {
			return false;
		}
	}
};

struct preservecmp {
	bool operator()(const struct coalesce &a, const struct coalesce &b) {
		return a.original_seq < b.original_seq;
	}
} preservecmp;

int coalcmp(const void *v1, const void *v2) {
	const struct coalesce *c1 = (const struct coalesce *) v1;
	const struct coalesce *c2 = (const struct coalesce *) v2;

	int cmp = c1->type - c2->type;
	if (cmp != 0) {
		return cmp;
	}

	return metacmp(c1->m, c1->keys, c1->values, c1->stringpool, c2->m, c2->keys, c2->values, c2->stringpool);
}

int coalindexcmp(const struct coalesce *c1, const struct coalesce *c2) {
	int cmp = coalcmp((const void *) c1, (const void *) c2);

	if (cmp == 0) {
		if (c1->index < c2->index) {
			return -1;
		} else if (c1->index > c2->index) {
			return 1;
		}

		if (c1->index2 > c2->index2) {
			return -1;
		} else if (c1->index2 < c2->index2) {
			return 1;
		}
	}

	return cmp;
}

mvt_value retrieve_string(long long off, char *stringpool, int *otype) {
	int type = stringpool[off];
	char *s = stringpool + off + 1;

	if (otype != NULL) {
		*otype = type;
	}

	mvt_value tv;
	if (type == VT_NUMBER) {
		long long v;
		if (is_integer(s, &v)) {
			if (v >= 0) {
				tv.type = mvt_int;
				tv.numeric_value.int_value = v;
			} else {
				tv.type = mvt_sint;
				tv.numeric_value.sint_value = v;
			}
		} else {
			tv.type = mvt_double;
			tv.numeric_value.double_value = atof(s);
		}
	} else if (type == VT_BOOLEAN) {
		tv.type = mvt_bool;
		tv.numeric_value.bool_value = (s[0] == 't');
	} else {
		tv.type = mvt_string;
		tv.string_value = s;
	}

	return tv;
}

void decode_meta(int m, std::vector<long long> &metakeys, std::vector<long long> &metavals, char *stringpool, mvt_layer &layer, mvt_feature &feature) {
	int i;
	for (i = 0; i < m; i++) {
		int otype;
		mvt_value key = retrieve_string(metakeys[i], stringpool, NULL);
		mvt_value value = retrieve_string(metavals[i], stringpool, &otype);

		layer.tag(feature, key.string_value, value);
	}
}

int metacmp(int m1, const std::vector<long long> &keys1, const std::vector<long long> &values1, char *stringpool1, int m2, const std::vector<long long> &keys2, const std::vector<long long> &values2, char *stringpool2) {
	// XXX
	// Ideally this would make identical features compare the same lexically
	// even if their attributes were declared in different orders in different instances.
	// In practice, this is probably good enough to put "identical" features together.

	int i;
	for (i = 0; i < m1 && i < m2; i++) {
		mvt_value key1 = retrieve_string(keys1[i], stringpool1, NULL);
		mvt_value key2 = retrieve_string(keys2[i], stringpool2, NULL);

		if (key1.string_value < key2.string_value) {
			return -1;
		} else if (key1.string_value > key2.string_value) {
			return 1;
		}

		long long off1 = values1[i];
		int type1 = stringpool1[off1];
		char *s1 = stringpool1 + off1 + 1;

		long long off2 = values2[i];
		int type2 = stringpool2[off2];
		char *s2 = stringpool2 + off2 + 1;

		if (type1 != type2) {
			return type1 - type2;
		}
		int cmp = strcmp(s1, s2);
		if (s1 != s2) {
			return cmp;
		}
	}

	if (m1 < m2) {
		return -1;
	} else if (m1 > m2) {
		return 1;
	} else {
		return 0;
	}
}

static int is_integer(const char *s, long long *v) {
	errno = 0;
	char *endptr;

	*v = strtoll(s, &endptr, 0);
	if (*v == 0 && errno != 0) {
		return 0;
	}
	if ((*v == LLONG_MIN || *v == LLONG_MAX) && (errno == ERANGE)) {
		return 0;
	}
	if (*endptr != '\0') {
		// Special case: If it is an integer followed by .0000 or similar,
		// it is still an integer

		if (*endptr != '.') {
			return 0;
		}
		endptr++;
		for (; *endptr != '\0'; endptr++) {
			if (*endptr != '0') {
				return 0;
			}
		}

		return 1;
	}

	return 1;
}

struct sll {
	char *name;
	long long val;

	bool operator<(const sll &o) const {
		if (this->val < o.val) {
			return true;
		} else {
			return false;
		}
	}

	sll(char *nname, long long nval) {
		this->name = nname;
		this->val = nval;
	}
};

void rewrite(drawvec &geom, int z, int nextzoom, int maxzoom, long long *bbox, unsigned tx, unsigned ty, int buffer, int line_detail, int *within, long long *geompos, FILE **geomfile, const char *fname, signed char t, int layer, long long metastart, signed char feature_minzoom, int child_shards, int max_zoom_increment, long long seq, int tippecanoe_minzoom, int tippecanoe_maxzoom, int segment, unsigned *initial_x, unsigned *initial_y, int m, std::vector<long long> &metakeys, std::vector<long long> &metavals, bool has_id, unsigned long long id) {
	if (geom.size() > 0 && nextzoom <= maxzoom) {
		int xo, yo;
		int span = 1 << (nextzoom - z);

		// Get the feature bounding box in pixel (256) coordinates at the child zoom
		// in order to calculate which sub-tiles it can touch including the buffer.
		long long bbox2[4];
		int k;
		for (k = 0; k < 4; k++) {
			// Division instead of right-shift because coordinates can be negative
			bbox2[k] = bbox[k] / (1 << (32 - nextzoom - 8));
		}
		// Decrement the top and left edges so that any features that are
		// touching the edge can potentially be included in the adjacent tiles too.
		bbox2[0] -= buffer + 1;
		bbox2[1] -= buffer + 1;
		bbox2[2] += buffer;
		bbox2[3] += buffer;

		for (k = 0; k < 4; k++) {
			if (bbox2[k] < 0) {
				bbox2[k] = 0;
			}
			if (bbox2[k] >= 256 * span) {
				bbox2[k] = 256 * (span - 1);
			}

			bbox2[k] /= 256;
		}

		for (xo = bbox2[0]; xo <= bbox2[2]; xo++) {
			for (yo = bbox2[1]; yo <= bbox2[3]; yo++) {
				unsigned jx = tx * span + xo;
				unsigned jy = ty * span + yo;

				// j is the shard that the child tile's data is being written to.
				//
				// Be careful: We can't jump more zoom levels than max_zoom_increment
				// because that could break the constraint that each of the children
				// of the current tile must have its own shard, because the data for
				// the child tile must be contiguous within the shard.
				//
				// But it's OK to spread children across all the shards, not just
				// the four that would normally result from splitting one tile,
				// because it will go through all the shards when it does the
				// next zoom.
				//
				// If child_shards is a power of 2 but not a power of 4, this will
				// shard X more widely than Y. XXX Is there a better way to do this
				// without causing collisions?

				int j = ((jx << max_zoom_increment) |
					 ((jy & ((1 << max_zoom_increment) - 1)))) &
					(child_shards - 1);

				{
					if (!within[j]) {
						serialize_int(geomfile[j], nextzoom, &geompos[j], fname);
						serialize_uint(geomfile[j], tx * span + xo, &geompos[j], fname);
						serialize_uint(geomfile[j], ty * span + yo, &geompos[j], fname);
						within[j] = 1;
					}

					// Offset from tile coordinates back to world coordinates
					unsigned sx = 0, sy = 0;
					if (z != 0) {
						sx = tx << (32 - z);
						sy = ty << (32 - z);
					}

					// printf("type %d, meta %lld\n", t, metastart);
					serialize_byte(geomfile[j], t, &geompos[j], fname);
					serialize_long_long(geomfile[j], seq, &geompos[j], fname);
					serialize_long_long(geomfile[j], (layer << 3) | (has_id << 2) | ((tippecanoe_minzoom != -1) << 1) | (tippecanoe_maxzoom != -1), &geompos[j], fname);
					if (tippecanoe_minzoom != -1) {
						serialize_int(geomfile[j], tippecanoe_minzoom, geompos, fname);
					}
					if (tippecanoe_maxzoom != -1) {
						serialize_int(geomfile[j], tippecanoe_maxzoom, geompos, fname);
					}
					if (has_id) {
						serialize_ulong_long(geomfile[j], id, geompos, fname);
					}
					serialize_int(geomfile[j], segment, &geompos[j], fname);
					long long wx = initial_x[segment], wy = initial_y[segment];

					for (size_t u = 0; u < geom.size(); u++) {
						serialize_byte(geomfile[j], geom[u].op, &geompos[j], fname);

						if (geom[u].op != VT_CLOSEPATH) {
							serialize_long_long(geomfile[j], ((geom[u].x + sx) >> geometry_scale) - (wx >> geometry_scale), &geompos[j], fname);
							serialize_long_long(geomfile[j], ((geom[u].y + sy) >> geometry_scale) - (wy >> geometry_scale), &geompos[j], fname);
							wx = geom[u].x + sx;
							wy = geom[u].y + sy;
						}
					}

					serialize_byte(geomfile[j], VT_END, &geompos[j], fname);

					serialize_int(geomfile[j], m, &geompos[j], fname);
					serialize_long_long(geomfile[j], metastart, &geompos[j], fname);
					if (metastart < 0) {
						for (int i = 0; i < m; i++) {
							serialize_long_long(geomfile[j], metakeys[i], &geompos[j], fname);
							serialize_long_long(geomfile[j], metavals[i], &geompos[j], fname);
						}
					}

					serialize_byte(geomfile[j], feature_minzoom, &geompos[j], fname);
				}
			}
		}
	}
}

struct partial {
	std::vector<drawvec> geoms;
	std::vector<long long> keys;
	std::vector<long long> values;
	char *meta;
	long long layer;
	long long original_seq;
	unsigned long long index;
	unsigned long long index2;
	int m;
	int segment;
	bool reduced;
	int z;
	int line_detail;
	int maxzoom;
	double spacing;
	double simplification;
	signed char t;
	unsigned long long id;
	bool has_id;
};

struct partial_arg {
	std::vector<struct partial> *partials;
	int task;
	int tasks;
};

drawvec revive_polygon(drawvec &geom, double area, int z, int detail) {
	// From area in world coordinates to area in tile coordinates
	long long divisor = 1LL << (32 - detail - z);
	area /= divisor * divisor;

	if (area == 0) {
		return drawvec();
	}

	int height = ceil(sqrt(area));
	int width = round(area / height);
	if (width == 0) {
		width = 1;
	}

	long long sx = 0, sy = 0, n = 0;
	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO || geom[i].op == VT_LINETO) {
			sx += geom[i].x;
			sy += geom[i].y;
			n++;
		}
	}

	if (n > 0) {
		sx /= n;
		sy /= n;

		drawvec out;
		out.push_back(draw(VT_MOVETO, sx - (width / 2), sy - (height / 2)));
		out.push_back(draw(VT_LINETO, sx - (width / 2) + width, sy - (height / 2)));
		out.push_back(draw(VT_LINETO, sx - (width / 2) + width, sy - (height / 2) + height));
		out.push_back(draw(VT_LINETO, sx - (width / 2), sy - (height / 2) + height));
		out.push_back(draw(VT_LINETO, sx - (width / 2), sy - (height / 2)));

		return out;
	} else {
		return drawvec();
	}
}

void *partial_feature_worker(void *v) {
	struct partial_arg *a = (struct partial_arg *) v;
	std::vector<struct partial> *partials = a->partials;

	for (size_t i = a->task; i < (*partials).size(); i += a->tasks) {
		drawvec geom = (*partials)[i].geoms[0];  // XXX assumption of a single geometry at the beginning
		(*partials)[i].geoms.clear();		 // avoid keeping two copies in memory
		signed char t = (*partials)[i].t;
		int z = (*partials)[i].z;
		int line_detail = (*partials)[i].line_detail;
		int maxzoom = (*partials)[i].maxzoom;

		double area = 0;
		if (t == VT_POLYGON) {
			area = get_area(geom, 0, geom.size());
		}

		if ((t == VT_LINE || t == VT_POLYGON) && !(prevent[P_SIMPLIFY] || (z == maxzoom && prevent[P_SIMPLIFY_LOW]))) {
			if (1 /* !reduced */) {  // XXX why did this not simplify if reduced?
				if (t == VT_LINE) {
					geom = remove_noop(geom, t, 32 - z - line_detail);
				}

				drawvec ngeom = simplify_lines(geom, z, line_detail, !(prevent[P_CLIPPING] || prevent[P_DUPLICATION]), (*partials)[i].simplification);

				if (t != VT_POLYGON || ngeom.size() >= 3) {
					geom = ngeom;
				}
			}
		}

#if 0
		if (t == VT_LINE && z != basezoom) {
			geom = shrink_lines(geom, z, line_detail, basezoom, &along);
		}
#endif

		if (t == VT_LINE && additional[A_REVERSE]) {
			geom = reorder_lines(geom);
		}

		to_tile_scale(geom, z, line_detail);

		std::vector<drawvec> geoms;
		geoms.push_back(geom);

		if (t == VT_POLYGON && !prevent[P_POLYGON_SPLIT]) {
			geoms = chop_polygon(geoms);
		}

		if (t == VT_POLYGON) {
			// Scaling may have made the polygon degenerate.
			// Give Clipper a chance to try to fix it.
			for (size_t g = 0; g < geoms.size(); g++) {
				drawvec before = geoms[g];
				geoms[g] = clean_or_clip_poly(geoms[g], 0, 0, 0, false);
				if (additional[A_DEBUG_POLYGON]) {
					check_polygon(geoms[g], before);
				}

				if (geoms[g].size() < 3) {
					geoms[g] = revive_polygon(before, area / geoms.size(), z, line_detail);
				}
			}
		}

		// Worth skipping this if not coalescing anyway?
		if (geoms.size() > 0 && geoms[0].size() > 0) {
			(*partials)[i].index = encode(geoms[0][0].x, geoms[0][0].y);
			(*partials)[i].index2 = encode(geoms[0][geoms[0].size() - 1].x, geoms[0][geoms[0].size() - 1].y);

			// Anything numbered below the start of the line
			// can't possibly be the next feature.
			// We want lowest-but-not-under.
			if ((*partials)[i].index2 < (*partials)[i].index) {
				(*partials)[i].index2 = ~0LL;
			}
		} else {
			(*partials)[i].index = 0;
			(*partials)[i].index2 = 0;
		}

		(*partials)[i].geoms = geoms;
	}

	return NULL;
}

int manage_gap(unsigned long long index, unsigned long long *previndex, double scale, double gamma, double *gap) {
	if (gamma > 0) {
		if (*gap > 0) {
			if (index == *previndex) {
				return 1;  // Exact duplicate: can't fulfil the gap requirement
			}

			if (index < *previndex || std::exp(std::log((index - *previndex) / scale) * gamma) >= *gap) {
				// Dot is further from the previous than the nth root of the gap,
				// so produce it, and choose a new gap at the next point.
				*gap = 0;
			} else {
				return 1;
			}
		} else if (index >= *previndex) {
			*gap = (index - *previndex) / scale;

			if (*gap == 0) {
				return 1;  // Exact duplicate: skip
			} else if (*gap < 1) {
				return 1;  // Narrow dot spacing: need to stretch out
			} else {
				*gap = 0;  // Wider spacing than minimum: so pass through unchanged
			}
		}

		*previndex = index;
	}

	return 0;
}

long long write_tile(FILE *geoms, long long *geompos_in, char *metabase, char *stringpool, int z, unsigned tx, unsigned ty, int detail, int min_detail, int basezoom, std::vector<std::string> *layernames, sqlite3 *outdb, double droprate, int buffer, const char *fname, FILE **geomfile, int minzoom, int maxzoom, double todo, volatile long long *along, long long alongminus, double gamma, int nlayers, int child_shards, long long *meta_off, long long *pool_off, unsigned *initial_x, unsigned *initial_y, volatile int *running, double simplification) {
	int line_detail;
	double fraction = 1;

	long long og = *geompos_in;

	// XXX is there a way to do this without floating point?
	int max_zoom_increment = std::log(child_shards) / std::log(4);
	if (child_shards < 4 || max_zoom_increment < 1) {
		fprintf(stderr, "Internal error: %d shards, max zoom increment %d\n", child_shards, max_zoom_increment);
		exit(EXIT_FAILURE);
	}
	if ((((child_shards - 1) << 1) & child_shards) != child_shards) {
		fprintf(stderr, "Internal error: %d shards not a power of 2\n", child_shards);
		exit(EXIT_FAILURE);
	}

	int nextzoom = z + 1;
	if (nextzoom < minzoom) {
		if (z + max_zoom_increment > minzoom) {
			nextzoom = minzoom;
		} else {
			nextzoom = z + max_zoom_increment;
		}
	}

	static volatile double oprogress = 0;

	// This only loops if the tile data didn't fit, in which case the detail
	// goes down and the progress indicator goes backward for the next try.
	for (line_detail = detail; line_detail >= min_detail || line_detail == detail; line_detail--, oprogress = 0) {
		long long count = 0;
		double accum_area = 0;

		double interval = 0;
		double seq = 0;
		if (z < basezoom) {
			interval = std::exp(std::log(droprate) * (basezoom - z));
		}

		double fraction_accum = 0;

		unsigned long long previndex = 0, density_previndex = 0;
		double scale = (double) (1LL << (64 - 2 * (z + 8)));
		double gap = 0, density_gap = 0;
		double spacing = 0;

		long long original_features = 0;
		long long unclipped_features = 0;

		std::vector<struct partial> partials;
		std::vector<std::vector<coalesce> > features;
		for (int i = 0; i < nlayers; i++) {
			features.push_back(std::vector<coalesce>());
		}

		int within[child_shards];
		long long geompos[child_shards];
		memset(within, '\0', sizeof(within));
		memset(geompos, '\0', sizeof(geompos));

		if (*geompos_in != og) {
			if (fseek(geoms, og, SEEK_SET) != 0) {
				perror("fseek geom");
				exit(EXIT_FAILURE);
			}
			*geompos_in = og;
		}

		while (1) {
			signed char t;
			deserialize_byte_io(geoms, &t, geompos_in);
			if (t < 0) {
				break;
			}

			long long original_seq;
			deserialize_long_long_io(geoms, &original_seq, geompos_in);

			long long layer;
			deserialize_long_long_io(geoms, &layer, geompos_in);
			int tippecanoe_minzoom = -1, tippecanoe_maxzoom = -1;
			unsigned long long id = 0;
			bool has_id = false;
			if (layer & 2) {
				deserialize_int_io(geoms, &tippecanoe_minzoom, geompos_in);
			}
			if (layer & 1) {
				deserialize_int_io(geoms, &tippecanoe_maxzoom, geompos_in);
			}
			if (layer & 4) {
				has_id = true;
				deserialize_ulong_long_io(geoms, &id, geompos_in);
			}
			layer >>= 3;

			int segment;
			deserialize_int_io(geoms, &segment, geompos_in);

			long long bbox[4];

			drawvec geom = decode_geometry(geoms, geompos_in, z, tx, ty, line_detail, bbox, initial_x[segment], initial_y[segment]);

			long long metastart;
			int m;
			deserialize_int_io(geoms, &m, geompos_in);
			deserialize_long_long_io(geoms, &metastart, geompos_in);
			char *meta = NULL;
			std::vector<long long> metakeys, metavals;

			if (metastart >= 0) {
				meta = metabase + metastart + meta_off[segment];

				for (int i = 0; i < m; i++) {
					long long k, v;
					deserialize_long_long(&meta, &k);
					deserialize_long_long(&meta, &v);
					metakeys.push_back(k);
					metavals.push_back(v);
				}
			} else {
				for (int i = 0; i < m; i++) {
					long long k, v;
					deserialize_long_long_io(geoms, &k, geompos_in);
					deserialize_long_long_io(geoms, &v, geompos_in);
					metakeys.push_back(k);
					metavals.push_back(v);
				}
			}

			signed char feature_minzoom;
			deserialize_byte_io(geoms, &feature_minzoom, geompos_in);

			double progress = floor((((*geompos_in + *along - alongminus) / (double) todo) + z) / (maxzoom + 1) * 1000) / 10;
			if (progress >= oprogress + 0.1) {
				if (!quiet) {
					fprintf(stderr, "  %3.1f%%  %d/%u/%u  \r", progress, z, tx, ty);
				}
				oprogress = progress;
			}

			original_features++;

			if (z == 0 && t == VT_POLYGON) {
				geom = fix_polygon(geom);
			}

			int quick = quick_check(bbox, z, line_detail, buffer);
			if (quick == 0) {
				continue;
			}

			if (z == 0) {
				if (bbox[0] < 0 || bbox[2] > 1LL << 32) {
					// If the geometry extends off the edge of the world, concatenate on another copy
					// shifted by 360 degrees, and then make sure both copies get clipped down to size.

					size_t n = geom.size();

					if (bbox[0] < 0) {
						for (size_t i = 0; i < n; i++) {
							geom.push_back(draw(geom[i].op, geom[i].x + (1LL << 32), geom[i].y));
						}
					}

					if (bbox[2] > 1LL << 32) {
						for (size_t i = 0; i < n; i++) {
							geom.push_back(draw(geom[i].op, geom[i].x - (1LL << 32), geom[i].y));
						}
					}

					bbox[0] = 0;
					bbox[2] = 1LL << 32;

					quick = -1;
				}
			}

			// Can't accept the quick check if guaranteeing no duplication, since the
			// overlap might have been in the buffer.
			if (quick != 1 || prevent[P_DUPLICATION]) {
				drawvec clipped;

				// Do the clipping, even if we are going to include the whole feature,
				// so that we can know whether the feature itself, or only the feature's
				// bounding box, touches the tile.

				if (t == VT_LINE) {
					clipped = clip_lines(geom, z, line_detail, buffer);
				}
				if (t == VT_POLYGON) {
					clipped = simple_clip_poly(geom, z, line_detail, buffer);
				}
				if (t == VT_POINT) {
					clipped = clip_point(geom, z, line_detail, buffer);
				}

				clipped = remove_noop(clipped, t, 0);

				// Must clip at z0 even if we don't want clipping, to handle features
				// that are duplicated across the date line

				if (prevent[P_DUPLICATION] && z != 0) {
					if (point_within_tile((bbox[0] + bbox[2]) / 2, (bbox[1] + bbox[3]) / 2, z, line_detail, buffer)) {
						// geom is unchanged
					} else {
						geom.clear();
					}
				} else if (prevent[P_CLIPPING] && z != 0) {
					if (clipped.size() == 0) {
						geom.clear();
					} else {
						// geom is unchanged
					}
				} else {
					geom = clipped;
				}
			}

			if (geom.size() > 0) {
				unclipped_features++;
			}

			if (line_detail == detail && fraction == 1) { /* only write out the next zoom once, even if we retry */
				rewrite(geom, z, nextzoom, maxzoom, bbox, tx, ty, buffer, line_detail, within, geompos, geomfile, fname, t, layer, metastart, feature_minzoom, child_shards, max_zoom_increment, original_seq, tippecanoe_minzoom, tippecanoe_maxzoom, segment, initial_x, initial_y, m, metakeys, metavals, has_id, id);
			}

			if (z < minzoom) {
				continue;
			}

			if (tippecanoe_minzoom != -1 && z < tippecanoe_minzoom) {
				continue;
			}
			if (tippecanoe_maxzoom != -1 && z > tippecanoe_maxzoom) {
				continue;
			}

			if (t == VT_LINE && z + line_detail <= feature_minzoom) {
				continue;
			}

			if (t == VT_POINT && z < feature_minzoom && gamma < 0) {
				continue;
			}

			unsigned long long index = 0;
			if (additional[A_CALCULATE_FEATURE_DENSITY] || gamma > 0) {
				index = encode(bbox[0] / 2 + bbox[2] / 2, bbox[1] / 2 + bbox[3] / 2);
			}

			if (gamma >= 0 && (t == VT_POINT ||
					   (additional[A_LINE_DROP] && t == VT_LINE) ||
					   (additional[A_POLYGON_DROP] && t == VT_POLYGON))) {
				seq++;
				if (seq >= 0) {
					seq -= interval;
				} else {
					continue;
				}

				if (gamma > 0) {
					if (manage_gap(index, &previndex, scale, gamma, &gap)) {
						continue;
					}
				}
			}

			if (additional[A_CALCULATE_FEATURE_DENSITY]) {
				// Gamma is always 1 for this calculation so there is a reasonable
				// interpretation when no features are being dropped.
				// The spacing is only calculated if a feature would be retained by
				// that standard, so that duplicates aren't reported as infinitely dense.

				double o_density_previndex = density_previndex;
				if (!manage_gap(index, &density_previndex, scale, 1, &density_gap)) {
					spacing = (index - o_density_previndex) / scale;
				}
			}

			fraction_accum += fraction;
			if (fraction_accum < 1) {
				continue;
			}
			fraction_accum -= 1;

			bool reduced = false;
			if (t == VT_POLYGON) {
				geom = reduce_tiny_poly(geom, z, line_detail, &reduced, &accum_area);
			}

			if (geom.size() > 0) {
				partial p;
				p.geoms.push_back(geom);
				p.layer = layer;
				p.m = m;
				p.meta = meta;
				p.t = t;
				p.segment = segment;
				p.original_seq = original_seq;
				p.reduced = reduced;
				p.z = z;
				p.line_detail = line_detail;
				p.maxzoom = maxzoom;
				p.keys = metakeys;
				p.values = metavals;
				p.spacing = spacing;
				p.simplification = simplification;
				p.id = id;
				p.has_id = has_id;
				partials.push_back(p);
			}
		}

		int tasks = ceil((double) CPUS / *running);
		if (tasks < 1) {
			tasks = 1;
		}

		pthread_t pthreads[tasks];
		partial_arg args[tasks];
		for (int i = 0; i < tasks; i++) {
			args[i].task = i;
			args[i].tasks = tasks;
			args[i].partials = &partials;

			if (tasks > 1) {
				if (pthread_create(&pthreads[i], NULL, partial_feature_worker, &args[i]) != 0) {
					perror("pthread_create");
					exit(EXIT_FAILURE);
				}
			} else {
				partial_feature_worker(&args[i]);
			}
		}

		if (tasks > 1) {
			for (int i = 0; i < tasks; i++) {
				void *retval;

				if (pthread_join(pthreads[i], &retval) != 0) {
					perror("pthread_join");
				}
			}
		}

		for (size_t i = 0; i < partials.size(); i++) {
			std::vector<drawvec> &pgeoms = partials[i].geoms;
			long long layer = partials[i].layer;
			signed char t = partials[i].t;
			long long original_seq = partials[i].original_seq;

			// A complex polygon may have been split up into multiple geometries.
			// Break them out into multiple features if necessary.
			for (size_t j = 0; j < pgeoms.size(); j++) {
				if (t == VT_POINT || draws_something(pgeoms[j])) {
					struct coalesce c;

					c.type = t;
					c.index = partials[i].index;
					c.index2 = partials[i].index2;
					c.geom = pgeoms[j];
					pgeoms[j].clear();
					c.coalesced = false;
					c.original_seq = original_seq;
					c.m = partials[i].m;
					c.meta = partials[i].meta;
					c.stringpool = stringpool + pool_off[partials[i].segment];
					c.keys = partials[i].keys;
					c.values = partials[i].values;
					c.spacing = partials[i].spacing;
					c.id = partials[i].id;
					c.has_id = partials[i].has_id;

					features[layer].push_back(c);
				}
			}
		}

		partials.clear();

		int j;
		for (j = 0; j < child_shards; j++) {
			if (within[j]) {
				serialize_byte(geomfile[j], -2, &geompos[j], fname);
				within[j] = 0;
			}
		}

		for (j = 0; j < nlayers; j++) {
			if (additional[A_REORDER]) {
				std::sort(features[j].begin(), features[j].end());
			}

			std::vector<coalesce> out;
			if (features[j].size() > 0) {
				out.push_back(features[j][0]);
			}
			for (size_t x = 1; x < features[j].size(); x++) {
				size_t y = out.size() - 1;

#if 0
				if (out.size() > 0 && coalcmp(&features[j][x], &out[y]) < 0) {
					fprintf(stderr, "\nfeature out of order\n");
				}
#endif

				if (additional[A_COALESCE] && out.size() > 0 && out[y].geom.size() + features[j][x].geom.size() < 700 && coalcmp(&features[j][x], &out[y]) == 0 && features[j][x].type != VT_POINT) {
					for (size_t g = 0; g < features[j][x].geom.size(); g++) {
						out[y].geom.push_back(features[j][x].geom[g]);
					}
					out[y].coalesced = true;
				} else {
					out.push_back(features[j][x]);
				}
			}

			features[j] = out;

			out.clear();
			for (size_t x = 0; x < features[j].size(); x++) {
				if (features[j][x].coalesced && features[j][x].type == VT_LINE) {
					features[j][x].geom = remove_noop(features[j][x].geom, features[j][x].type, 0);
					features[j][x].geom = simplify_lines(features[j][x].geom, 32, 0,
									     !(prevent[P_CLIPPING] || prevent[P_DUPLICATION]), simplification);
				}

				if (features[j][x].type == VT_POLYGON) {
					if (features[j][x].coalesced) {
						features[j][x].geom = clean_or_clip_poly(features[j][x].geom, 0, 0, 0, false);
					}

					features[j][x].geom = close_poly(features[j][x].geom);
				}

				if (features[j][x].geom.size() > 0) {
					out.push_back(features[j][x]);
				}
			}
			features[j] = out;

			if (prevent[P_INPUT_ORDER]) {
				std::sort(features[j].begin(), features[j].end(), preservecmp);
			}
		}

		mvt_tile tile;

		for (size_t k = 0; k < features.size(); k++) {
			mvt_layer layer;

			layer.name = (*layernames)[k];
			layer.version = 2;
			layer.extent = 1 << line_detail;

			for (size_t x = 0; x < features[k].size(); x++) {
				mvt_feature feature;

				if (features[k][x].type == VT_LINE || features[k][x].type == VT_POLYGON) {
					features[k][x].geom = remove_noop(features[k][x].geom, features[k][x].type, 0);
				}

				if (features[k][x].geom.size() == 0) {
					continue;
				}

				feature.type = features[k][x].type;
				feature.geometry = to_feature(features[k][x].geom);
				count += features[k][x].geom.size();
				features[k][x].geom.clear();

				feature.id = features[k][x].id;
				feature.has_id = features[k][x].has_id;

				decode_meta(features[k][x].m, features[k][x].keys, features[k][x].values, features[k][x].stringpool, layer, feature);

				if (additional[A_CALCULATE_FEATURE_DENSITY]) {
					int glow = 255;
					if (features[k][x].spacing > 0) {
						glow = (1 / features[k][x].spacing);
						if (glow > 255) {
							glow = 255;
						}
					}
					mvt_value v;
					v.type = mvt_sint;
					v.numeric_value.sint_value = glow;
					layer.tag(feature, "tippecanoe_feature_density", v);
				}

				layer.features.push_back(feature);
			}

			if (layer.features.size() > 0) {
				tile.layers.push_back(layer);
			}
		}

		if (z == 0 && unclipped_features < original_features / 2) {
			fprintf(stderr, "\n\nMore than half the features were clipped away at zoom level 0.\n");
			fprintf(stderr, "Is your data in the wrong projection? It should be in WGS84/EPSG:4326.\n");
		}

		long long totalsize = 0;
		for (j = 0; j < nlayers; j++) {
			totalsize += features[j].size();
		}

		double progress = floor((((*geompos_in + *along - alongminus) / (double) todo) + z) / (maxzoom + 1) * 1000) / 10;
		if (progress >= oprogress + 0.1) {
			if (!quiet) {
				fprintf(stderr, "  %3.1f%%  %d/%u/%u  \r", progress, z, tx, ty);
			}
			oprogress = progress;
		}

		if (totalsize > 0 && tile.layers.size() > 0) {
			if (totalsize > 200000 && !prevent[P_FEATURE_LIMIT]) {
				fprintf(stderr, "tile %d/%u/%u has %lld features, >200000    \n", z, tx, ty, totalsize);
				fprintf(stderr, "Try using -B to set a higher base zoom level.\n");
				return -1;
			}

			std::string compressed = tile.encode();

			if (compressed.size() > 500000 && !prevent[P_KILOBYTE_LIMIT]) {
				if (!quiet) {
					fprintf(stderr, "tile %d/%u/%u size is %lld with detail %d, >500000    \n", z, tx, ty, (long long) compressed.size(), line_detail);
				}

				if (prevent[P_DYNAMIC_DROP]) {
					// The 95% is a guess to avoid too many retries
					// and probably actually varies based on how much duplicated metadata there is

					fraction = fraction * 500000 / compressed.size() * 0.95;
					if (!quiet) {
						fprintf(stderr, "Going to try keeping %0.2f%% of the features to make it fit\n", fraction * 100);
					}
					line_detail++;  // to keep it the same when the loop decrements it
				}
			} else {
				if (pthread_mutex_lock(&db_lock) != 0) {
					perror("pthread_mutex_lock");
					exit(EXIT_FAILURE);
				}

				mbtiles_write_tile(outdb, z, tx, ty, compressed.data(), compressed.size());

				if (pthread_mutex_unlock(&db_lock) != 0) {
					perror("pthread_mutex_unlock");
					exit(EXIT_FAILURE);
				}

				return count;
			}
		} else {
			return count;
		}
	}

	fprintf(stderr, "could not make tile %d/%u/%u small enough\n", z, tx, ty);
	return -1;
}

struct task {
	int fileno;
	struct task *next;
};

struct write_tile_args {
	struct task *tasks;
	char *metabase;
	char *stringpool;
	int min_detail;
	int basezoom;
	std::vector<std::string> *layernames;
	sqlite3 *outdb;
	double droprate;
	int buffer;
	const char *fname;
	FILE **geomfile;
	double todo;
	volatile long long *along;
	double gamma;
	int nlayers;
	int child_shards;
	int *geomfd;
	off_t *geom_size;
	volatile unsigned *midx;
	volatile unsigned *midy;
	int maxzoom;
	int minzoom;
	int full_detail;
	int low_detail;
	double simplification;
	volatile long long *most;
	long long *meta_off;
	long long *pool_off;
	unsigned *initial_x;
	unsigned *initial_y;
	volatile int *running;
	int err;
};

void *run_thread(void *vargs) {
	write_tile_args *arg = (write_tile_args *) vargs;
	struct task *task;

	for (task = arg->tasks; task != NULL; task = task->next) {
		int j = task->fileno;

		if (arg->geomfd[j] < 0) {
			// only one source file for zoom level 0
			continue;
		}
		if (arg->geom_size[j] == 0) {
			continue;
		}

		// printf("%lld of geom_size\n", (long long) geom_size[j]);

		FILE *geom = fdopen(arg->geomfd[j], "rb");
		if (geom == NULL) {
			perror("mmap geom");
			exit(EXIT_FAILURE);
		}

		long long geompos = 0;
		long long prevgeom = 0;

		while (1) {
			int z;
			unsigned x, y;

			if (!deserialize_int_io(geom, &z, &geompos)) {
				break;
			}
			deserialize_uint_io(geom, &x, &geompos);
			deserialize_uint_io(geom, &y, &geompos);

			// fprintf(stderr, "%d/%u/%u\n", z, x, y);

			long long len = write_tile(geom, &geompos, arg->metabase, arg->stringpool, z, x, y, z == arg->maxzoom ? arg->full_detail : arg->low_detail, arg->min_detail, arg->basezoom, arg->layernames, arg->outdb, arg->droprate, arg->buffer, arg->fname, arg->geomfile, arg->minzoom, arg->maxzoom, arg->todo, arg->along, geompos, arg->gamma, arg->nlayers, arg->child_shards, arg->meta_off, arg->pool_off, arg->initial_x, arg->initial_y, arg->running, arg->simplification);

			if (len < 0) {
				int *err = &arg->err;
				*err = z - 1;
				return err;
			}

			if (pthread_mutex_lock(&var_lock) != 0) {
				perror("pthread_mutex_lock");
				exit(EXIT_FAILURE);
			}

			if (z == arg->maxzoom) {
				if (len > *arg->most) {
					*arg->midx = x;
					*arg->midy = y;
					*arg->most = len;
				} else if (len == *arg->most) {
					unsigned long long a = (((unsigned long long) x) << 32) | y;
					unsigned long long b = (((unsigned long long) *arg->midx) << 32) | *arg->midy;

					if (a < b) {
						*arg->midx = x;
						*arg->midy = y;
						*arg->most = len;
					}
				}
			}

			*arg->along += geompos - prevgeom;
			prevgeom = geompos;

			if (pthread_mutex_unlock(&var_lock) != 0) {
				perror("pthread_mutex_unlock");
				exit(EXIT_FAILURE);
			}
		}

		if (fclose(geom) != 0) {
			perror("close geom");
			exit(EXIT_FAILURE);
		}
		// Since the fclose() has closed the underlying file descriptor
		arg->geomfd[j] = -1;
	}

	arg->running--;
	return NULL;
}

int traverse_zooms(int *geomfd, off_t *geom_size, char *metabase, char *stringpool, unsigned *midx, unsigned *midy, std::vector<std::string> &layernames, int maxzoom, int minzoom, int basezoom, sqlite3 *outdb, double droprate, int buffer, const char *fname, const char *tmpdir, double gamma, int nlayers, int full_detail, int low_detail, int min_detail, long long *meta_off, long long *pool_off, unsigned *initial_x, unsigned *initial_y, double simplification) {
	int i;
	for (i = 0; i <= maxzoom; i++) {
		long long most = 0;

		FILE *sub[TEMP_FILES];
		int subfd[TEMP_FILES];
		int j;
		for (j = 0; j < TEMP_FILES; j++) {
			char geomname[strlen(tmpdir) + strlen("/geom.XXXXXXXX" XSTRINGIFY(INT_MAX)) + 1];
			sprintf(geomname, "%s/geom%d.XXXXXXXX", tmpdir, j);
			subfd[j] = mkstemp(geomname);
			// printf("%s\n", geomname);
			if (subfd[j] < 0) {
				perror(geomname);
				exit(EXIT_FAILURE);
			}
			sub[j] = fopen(geomname, "wb");
			if (sub[j] == NULL) {
				perror(geomname);
				exit(EXIT_FAILURE);
			}
			unlink(geomname);
		}

		int useful_threads = 0;
		long long todo = 0;
		long long along = 0;
		for (j = 0; j < TEMP_FILES; j++) {
			todo += geom_size[j];
			if (geom_size[j] > 0) {
				useful_threads++;
			}
		}

		int threads = CPUS;
		if (threads > TEMP_FILES / 4) {
			threads = TEMP_FILES / 4;
		}
		// XXX is it useful to divide further if we know we are skipping
		// some zoom levels? Is it faster to have fewer CPUs working on
		// sharding, but more deeply, or fewer CPUs, less deeply?
		if (threads > useful_threads) {
			threads = useful_threads;
		}

		// Round down to a power of 2
		for (int e = 0; e < 30; e++) {
			if (threads >= (1 << e) && threads < (1 << (e + 1))) {
				threads = 1 << e;
				break;
			}
		}
		if (threads >= (1 << 30)) {
			threads = 1 << 30;
		}

		// Assign temporary files to threads

		struct task tasks[TEMP_FILES];
		struct dispatch {
			struct task *tasks;
			long long todo;
			struct dispatch *next;
		} dispatches[threads];
		struct dispatch *dispatch_head = &dispatches[0];
		for (j = 0; j < threads; j++) {
			dispatches[j].tasks = NULL;
			dispatches[j].todo = 0;
			if (j + 1 < threads) {
				dispatches[j].next = &dispatches[j + 1];
			} else {
				dispatches[j].next = NULL;
			}
		}

		for (j = 0; j < TEMP_FILES; j++) {
			if (geom_size[j] == 0) {
				continue;
			}

			tasks[j].fileno = j;
			tasks[j].next = dispatch_head->tasks;
			dispatch_head->tasks = &tasks[j];
			dispatch_head->todo += geom_size[j];

			struct dispatch *here = dispatch_head;
			dispatch_head = dispatch_head->next;

			dispatch **d;
			for (d = &dispatch_head; *d != NULL; d = &((*d)->next)) {
				if (here->todo < (*d)->todo) {
					break;
				}
			}

			here->next = *d;
			*d = here;
		}

		pthread_t pthreads[threads];
		write_tile_args args[threads];
		int running = threads;

		int thread;
		for (thread = 0; thread < threads; thread++) {
			args[thread].metabase = metabase;
			args[thread].stringpool = stringpool;
			args[thread].min_detail = min_detail;
			args[thread].basezoom = basezoom;
			args[thread].layernames = &layernames;
			args[thread].outdb = outdb;  // locked with db_lock
			args[thread].droprate = droprate;
			args[thread].buffer = buffer;
			args[thread].fname = fname;
			args[thread].geomfile = sub + thread * (TEMP_FILES / threads);
			args[thread].todo = todo;
			args[thread].along = &along;  // locked with var_lock
			args[thread].gamma = gamma;
			args[thread].nlayers = nlayers;
			args[thread].child_shards = TEMP_FILES / threads;
			args[thread].simplification = simplification;

			args[thread].geomfd = geomfd;
			args[thread].geom_size = geom_size;
			args[thread].midx = midx;  // locked with var_lock
			args[thread].midy = midy;  // locked with var_lock
			args[thread].maxzoom = maxzoom;
			args[thread].minzoom = minzoom;
			args[thread].full_detail = full_detail;
			args[thread].low_detail = low_detail;
			args[thread].most = &most;  // locked with var_lock
			args[thread].meta_off = meta_off;
			args[thread].pool_off = pool_off;
			args[thread].initial_x = initial_x;
			args[thread].initial_y = initial_y;

			args[thread].tasks = dispatches[thread].tasks;
			args[thread].running = &running;

			if (pthread_create(&pthreads[thread], NULL, run_thread, &args[thread]) != 0) {
				perror("pthread_create");
				exit(EXIT_FAILURE);
			}
		}

		int err = INT_MAX;

		for (thread = 0; thread < threads; thread++) {
			void *retval;

			if (pthread_join(pthreads[thread], &retval) != 0) {
				perror("pthread_join");
			}

			if (retval != NULL) {
				err = *((int *) retval);
			}
		}

		for (j = 0; j < TEMP_FILES; j++) {
			// Can be < 0 if there is only one source file, at z0
			if (geomfd[j] >= 0) {
				if (close(geomfd[j]) != 0) {
					perror("close geom");
					exit(EXIT_FAILURE);
				}
			}
			if (fclose(sub[j]) != 0) {
				perror("close subfile");
				exit(EXIT_FAILURE);
			}

			struct stat geomst;
			if (fstat(subfd[j], &geomst) != 0) {
				perror("stat geom\n");
				exit(EXIT_FAILURE);
			}

			geomfd[j] = subfd[j];
			geom_size[j] = geomst.st_size;
		}

		if (err != INT_MAX) {
			return err;
		}
	}

	int j;
	for (j = 0; j < TEMP_FILES; j++) {
		// Can be < 0 if there is only one source file, at z0
		if (geomfd[j] >= 0) {
			if (close(geomfd[j]) != 0) {
				perror("close geom");
				exit(EXIT_FAILURE);
			}
		}
	}

	if (!quiet) {
		fprintf(stderr, "\n");
	}
	return maxzoom;
}
