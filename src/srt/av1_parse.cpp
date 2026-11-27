// AV1 → Enhanced-RTMP：上游 OBS 32.2 plugins/obs-outputs/rtmp-av1.c
// （obs_parse_av1_header / obs_parse_av1_packet）的逐字移植。
// 改动仅限：C++ 化；OBU 头与序列头解析改在零填充副本上进行（上游依赖堆冗余读，
// 合法输入下字节结果一致，非法输入不再越界）。
#include "srt/av1_parse.hpp"

#include <cassert>
#include <climits>
#include <cstring>

namespace Srt {
namespace {

// libobs 32.x util 帮助宏（系统头 30.x 没有）
inline int min_i32(int a, int b)
{
	return a < b ? a : b;
}

#define AV1_OBU_SEQUENCE_HEADER 1
#define AV1_OBU_TEMPORAL_DELIMITER 2
#define AV1_OBU_REDUNDANT_FRAME_HEADER 7
#define AV1_OBU_TILE_LIST 8
#define AV1_OBU_PADDING 15

#define AV1_OBU_TILE_GROUP 4
#define AV1_OBU_FRAME 6
#define AV1_OBU_FRAME_HEADER 3
#define AV1_OBU_METADATA 5

#define FF_PROFILE_AV1_MAIN 0
#define FF_PROFILE_AV1_HIGH 1
#define FF_PROFILE_AV1_PROFESSIONAL 2

enum frame_type {
	AV1_KEY_FRAME,
	AV1_INTER_FRAME,
	AV1_INTRA_FRAME,
	AV1_SWITCH_FRAME,
};

struct AV1SequenceParameters {
	uint8_t profile;
	uint8_t level;
	uint8_t tier;
	uint8_t bitdepth;
	uint8_t monochrome;
	uint8_t chroma_subsampling_x;
	uint8_t chroma_subsampling_y;
	uint8_t chroma_sample_position;
	uint8_t color_description_present_flag;
	uint8_t color_primaries;
	uint8_t transfer_characteristics;
	uint8_t matrix_coefficients;
	uint8_t color_range;
};

#define MAX_OBU_HEADER_SIZE (2 + 8)

struct Av1GetBitContext {
	const uint8_t *buffer, *buffer_end;
	int index;
	int size_in_bits;
	int size_in_bits_plus8;
};

inline int init_get_bits(Av1GetBitContext *s, const uint8_t *buffer, int bit_size)
{
	int buffer_size;
	int ret = 0;

	if (bit_size >= INT_MAX - 64 * 8 || bit_size < 0 || !buffer) {
		bit_size = 0;
		buffer = NULL;
		ret = -1;
	}

	buffer_size = (bit_size + 7) >> 3;

	s->buffer = buffer;
	s->size_in_bits = bit_size;
	s->size_in_bits_plus8 = bit_size + 8;
	s->buffer_end = buffer + buffer_size;
	s->index = 0;

	return ret;
}

inline int init_get_bits8(Av1GetBitContext *s, const uint8_t *buffer, int byte_size)
{
	if (byte_size > INT_MAX / 8 || byte_size < 0)
		byte_size = -1;
	return init_get_bits(s, buffer, byte_size * 8);
}

inline unsigned int get_bit1(Av1GetBitContext *s)
{
	unsigned int index = s->index;
	uint8_t result = s->buffer[index >> 3];

	result <<= index & 7;
	result >>= 8 - 1;

	if (s->index < s->size_in_bits_plus8)
		index++;
	s->index = index;

	return result;
}

inline unsigned int get_bits(Av1GetBitContext *s, unsigned int n)
{
	unsigned int out = 0;
	for (unsigned int i = 0; i < n; i++)
		out = (out << 1) | get_bit1(s);
	return out;
}

#define skip_bits get_bits

inline int get_bits_count(Av1GetBitContext *s)
{
	return s->index;
}

inline int get_bits_left(Av1GetBitContext *gb)
{
	return gb->size_in_bits - get_bits_count(gb);
}

#define get_bits_long get_bits
#define skip_bits_long get_bits_long

inline int64_t leb128(Av1GetBitContext *gb)
{
	int64_t ret = 0;
	int i;

	for (i = 0; i < 8; i++) {
		int byte = get_bits(gb, 8);
		ret |= (int64_t)(byte & 0x7f) << (i * 7);
		if (!(byte & 0x80))
			break;
	}
	return ret;
}

inline void uvlc(Av1GetBitContext *gb)
{
	int leading_zeros = 0;

	while (get_bits_left(gb)) {
		if (get_bits(gb, 1))
			break;
		leading_zeros++;
	}

	if (leading_zeros >= 32)
		return;

	skip_bits_long(gb, leading_zeros);
}

// 上游直接在调用方缓冲上读（最多 10 字节头 + 逐位越界裕量依赖堆冗余）；
// 这里复制到零填充的本地缓冲，合法输入下结果一致。
inline int parse_obu_header(const uint8_t *buf, int buf_size, int64_t *obu_size, int *start_pos, int *type,
			    int *temporal_id, int *spatial_id)
{
	uint8_t head[MAX_OBU_HEADER_SIZE + 8];
	int n = min_i32(buf_size, MAX_OBU_HEADER_SIZE);
	if (n < 0)
		n = 0;
	memcpy(head, buf, n);
	memset(head + n, 0, sizeof(head) - n);

	Av1GetBitContext gb;
	int ret, extension_flag, has_size_flag;
	size_t size;

	ret = init_get_bits8(&gb, head, n);
	if (ret < 0)
		return ret;

	if (get_bits(&gb, 1) != 0) // obu_forbidden_bit
		return -1;

	*type = get_bits(&gb, 4);
	extension_flag = get_bits(&gb, 1);
	has_size_flag = get_bits(&gb, 1);
	skip_bits(&gb, 1); // obu_reserved_1bit

	if (extension_flag) {
		*temporal_id = get_bits(&gb, 3);
		*spatial_id = get_bits(&gb, 2);
		skip_bits(&gb, 3); // extension_header_reserved_3bits
	} else {
		*temporal_id = *spatial_id = 0;
	}

	*obu_size = has_size_flag ? leb128(&gb) : buf_size - 1 - extension_flag;

	if (get_bits_left(&gb) < 0)
		return -1;

	*start_pos = get_bits_count(&gb) / 8;

	size = (size_t)(*obu_size + *start_pos);

	if (size > (size_t)buf_size)
		return -1;

	return (int)size;
}

inline int get_obu_bit_length(const uint8_t *buf, int size, int type)
{
	int v;

	/* There are no trailing bits on these */
	if (type == AV1_OBU_TILE_GROUP || type == AV1_OBU_TILE_LIST || type == AV1_OBU_FRAME) {
		if (size > INT_MAX / 8)
			return -1;
		else
			return size * 8;
	}

	while (size > 0 && buf[size - 1] == 0)
		size--;

	if (!size)
		return 0;

	v = buf[size - 1];

	if (size > INT_MAX / 8)
		return -1;
	size *= 8;

	/* Remove the trailing_one_bit and following trailing zeros */
	if (v)
		size -= __builtin_ctz((unsigned)v) + 1;

	return size;
}

int parse_color_config(AV1SequenceParameters *seq_params, Av1GetBitContext *gb)
{
	int twelve_bit = 0;
	int high_bitdepth = get_bits(gb, 1);
	if (seq_params->profile == FF_PROFILE_AV1_PROFESSIONAL && high_bitdepth)
		twelve_bit = get_bits(gb, 1);

	seq_params->bitdepth = 8 + (high_bitdepth * 2) + (twelve_bit * 2);

	if (seq_params->profile == FF_PROFILE_AV1_HIGH)
		seq_params->monochrome = 0;
	else
		seq_params->monochrome = get_bits(gb, 1);

	seq_params->color_description_present_flag = get_bits(gb, 1);
	if (seq_params->color_description_present_flag) {
		seq_params->color_primaries = get_bits(gb, 8);
		seq_params->transfer_characteristics = get_bits(gb, 8);
		seq_params->matrix_coefficients = get_bits(gb, 8);
	} else {
		seq_params->color_primaries = 2;
		seq_params->transfer_characteristics = 2;
		seq_params->matrix_coefficients = 2;
	}

	if (seq_params->monochrome) {
		seq_params->color_range = get_bits(gb, 1);
		seq_params->chroma_subsampling_x = 1;
		seq_params->chroma_subsampling_y = 1;
		seq_params->chroma_sample_position = 0;
		return 0;
	} else if (seq_params->color_primaries == 1 && seq_params->transfer_characteristics == 13 &&
		   seq_params->matrix_coefficients == 0) {
		seq_params->chroma_subsampling_x = 0;
		seq_params->chroma_subsampling_y = 0;
	} else {
		seq_params->color_range = get_bits(gb, 1);

		if (seq_params->profile == FF_PROFILE_AV1_MAIN) {
			seq_params->chroma_subsampling_x = 1;
			seq_params->chroma_subsampling_y = 1;
		} else if (seq_params->profile == FF_PROFILE_AV1_HIGH) {
			seq_params->chroma_subsampling_x = 0;
			seq_params->chroma_subsampling_y = 0;
		} else {
			if (twelve_bit) {
				seq_params->chroma_subsampling_x = get_bits(gb, 1);
				if (seq_params->chroma_subsampling_x)
					seq_params->chroma_subsampling_y = get_bits(gb, 1);
				else
					seq_params->chroma_subsampling_y = 0;
			} else {
				seq_params->chroma_subsampling_x = 1;
				seq_params->chroma_subsampling_y = 0;
			}
		}
		if (seq_params->chroma_subsampling_x && seq_params->chroma_subsampling_y)
			seq_params->chroma_sample_position = get_bits(gb, 2);
	}

	skip_bits(gb, 1); // separate_uv_delta_q

	return 0;
}

int parse_sequence_header(AV1SequenceParameters *seq_params, const uint8_t *buf, int size)
{
	Av1GetBitContext gb;
	int reduced_still_picture_header;
	int frame_width_bits_minus_1, frame_height_bits_minus_1;
	int size_bits, ret;

	size_bits = get_obu_bit_length(buf, size, AV1_OBU_SEQUENCE_HEADER);
	if (size_bits < 0)
		return size_bits;

	ret = init_get_bits(&gb, buf, size_bits);
	if (ret < 0)
		return ret;

	memset(seq_params, 0, sizeof(*seq_params));

	seq_params->profile = get_bits(&gb, 3);

	skip_bits(&gb, 1); // still_picture
	reduced_still_picture_header = get_bits(&gb, 1);

	if (reduced_still_picture_header) {
		seq_params->level = get_bits(&gb, 5);
		seq_params->tier = 0;
	} else {
		int initial_display_delay_present_flag, operating_points_cnt_minus_1;
		int decoder_model_info_present_flag, buffer_delay_length_minus_1;

		if (get_bits(&gb, 1))          // timing_info_present_flag
		{
			skip_bits_long(&gb, 32); // num_units_in_display_tick
			skip_bits_long(&gb, 32); // time_scale

			if (get_bits(&gb, 1)) // equal_picture_interval
				uvlc(&gb);    // num_ticks_per_picture_minus_1

			decoder_model_info_present_flag = get_bits(&gb, 1);
			if (decoder_model_info_present_flag) {
				buffer_delay_length_minus_1 = get_bits(&gb, 5);
				skip_bits_long(&gb, 32);
				skip_bits(&gb, 10);
			}
		} else
			decoder_model_info_present_flag = 0;

		initial_display_delay_present_flag = get_bits(&gb, 1);

		operating_points_cnt_minus_1 = get_bits(&gb, 5);
		for (int i = 0; i <= operating_points_cnt_minus_1; i++) {
			int seq_level_idx, seq_tier;

			skip_bits(&gb, 12);
			seq_level_idx = get_bits(&gb, 5);

			if (seq_level_idx > 7)
				seq_tier = get_bits(&gb, 1);
			else
				seq_tier = 0;

			if (decoder_model_info_present_flag) {
				if (get_bits(&gb, 1)) {
					skip_bits_long(&gb, buffer_delay_length_minus_1 + 1);
					skip_bits_long(&gb, buffer_delay_length_minus_1 + 1);
					skip_bits(&gb, 1);
				}
			}

			if (initial_display_delay_present_flag) {
				if (get_bits(&gb, 1))
					skip_bits(&gb, 4);
			}

			if (i == 0) {
				seq_params->level = seq_level_idx;
				seq_params->tier = seq_tier;
			}
		}
	}

	frame_width_bits_minus_1 = get_bits(&gb, 4);
	frame_height_bits_minus_1 = get_bits(&gb, 4);

	skip_bits(&gb, frame_width_bits_minus_1 + 1); // max_frame_width_minus_1
	skip_bits(&gb,
		  frame_height_bits_minus_1 + 1); // max_frame_height_minus_1

	if (!reduced_still_picture_header) {
		if (get_bits(&gb, 1)) // frame_id_numbers_present_flag
			skip_bits(&gb, 7);
	}

	skip_bits(&gb,
		  3); // use_128x128_superblock (1), enable_filter_intra (1), enable_intra_edge_filter (1)

	if (!reduced_still_picture_header) {
		int enable_order_hint, seq_force_screen_content_tools;

		skip_bits(&gb, 4);

		enable_order_hint = get_bits(&gb, 1);
		if (enable_order_hint)
			skip_bits(&gb, 2);

		if (get_bits(&gb, 1)) // seq_choose_screen_content_tools
			seq_force_screen_content_tools = 2;
		else
			seq_force_screen_content_tools = get_bits(&gb, 1);

		if (seq_force_screen_content_tools) {
			if (!get_bits(&gb, 1))     // seq_choose_integer_mv
				skip_bits(&gb, 1); // seq_force_integer_mv
		}

		if (enable_order_hint)
			skip_bits(&gb, 3); // order_hint_bits_minus_1
	}

	skip_bits(&gb, 3);

	parse_color_config(seq_params, &gb);

	skip_bits(&gb, 1); // film_grain_params_present

	if (get_bits_left(&gb))
		return -1;

	return 0;
}

void compute_av1_keyframe_priority(const uint8_t *buf, bool *is_keyframe, int *priority)
{
	/* Skip if the packet already has a priority, e.g., assigned by the
	 * encoder implementation (currently QSV/AMF). */
	if (*priority)
		return;

	Av1GetBitContext gb;

	init_get_bits8(&gb, buf, 1);

	// show_existing_frame
	if (get_bit1(&gb))
		return;

	enum frame_type type = (frame_type)get_bits(&gb, 2);
	bool show_frame;
	show_frame = get_bit1(&gb) != 0;

	switch (type) {
	case AV1_KEY_FRAME:
		*is_keyframe = true;
		*priority = 3;
		break;
	case AV1_INTER_FRAME:
		*priority = show_frame ? 1 : 2;
		break;
	case AV1_INTRA_FRAME:
		*priority = 3;
		break;
	case AV1_SWITCH_FRAME:
		*priority = 2;
		break;
	}
}

void serialize_av1_data(const uint8_t *data, size_t size, bool *is_keyframe, int *priority,
			std::vector<uint8_t> &out)
{
	const uint8_t *buf = data;
	const uint8_t *end = data + size;

	while (buf < end) {
		int64_t obu_size;
		int start_pos, type, temporal_id, spatial_id;
		int len = parse_obu_header(buf, (int)(end - buf), &obu_size, &start_pos, &type, &temporal_id,
					   &spatial_id);
		if (len < 0)
			return;

		switch (type) {
		case AV1_OBU_TEMPORAL_DELIMITER:
		case AV1_OBU_REDUNDANT_FRAME_HEADER:
		case AV1_OBU_TILE_LIST:
			break;
		case AV1_OBU_FRAME:
		case AV1_OBU_FRAME_HEADER:
			if (len > start_pos)
				compute_av1_keyframe_priority(buf + start_pos, is_keyframe, priority);
			/* Falls through. */
			[[fallthrough]];
		default:
			out.insert(out.end(), buf, buf + len);
			break;
		}
		buf += len;
	}
}

} // namespace

bool BuildAv1CodecConfRecord(const uint8_t *data, size_t size, std::vector<uint8_t> &out)
{
	if (!data || size == 0)
		return false;

	if (data[0] & 0x80) {
		int config_record_version = data[0] & 0x7f;
		if (config_record_version != 1 || size < 4)
			return false;

		out.assign(data, data + size);
		return true;
	}

	// AV1S init（上游 obs_parse_av1_header 主路径）
	AV1SequenceParameters seq_params;
	int nb_seq = 0, seq_size = 0, meta_size = 0;
	const uint8_t *seq = 0, *meta = 0;

	const uint8_t *buf = data;
	while (size > 0) {
		int64_t obu_size;
		int start_pos, type, temporal_id, spatial_id;
		int len = parse_obu_header(buf, (int)size, &obu_size, &start_pos, &type, &temporal_id, &spatial_id);
		if (len < 0)
			return false;

		switch (type) {
		case AV1_OBU_SEQUENCE_HEADER:
			nb_seq++;
			if (!obu_size || nb_seq > 1) {
				return false;
			}
			{
				// 上游在 OBU 载荷上原位解析并容忍 ≤7 字节尾部过读；这里复制 + 8 零填充。
				std::vector<uint8_t> padded((size_t)obu_size + 8, 0);
				memcpy(padded.data(), buf + start_pos, (size_t)obu_size);
				if (parse_sequence_header(&seq_params, padded.data(), (int)obu_size) < 0)
					return false;
			}

			seq = buf;
			seq_size = len;
			break;
		case AV1_OBU_METADATA:
			if (!obu_size)
				return false;
			meta = buf;
			meta_size = len;
			break;
		default:
			break;
		}
		size -= len;
		buf += len;
	}

	if (!nb_seq)
		return false;

	uint8_t av1header[4];
	av1header[0] = (1 << 7) | 1; // marker and version
	av1header[1] = (seq_params.profile << 5) | (seq_params.level);
	av1header[2] = (seq_params.tier << 7) | (seq_params.bitdepth > 8) << 6 | (seq_params.bitdepth == 12) << 5 |
		       (seq_params.monochrome) << 4 | (seq_params.chroma_subsampling_x) << 3 |
		       (seq_params.chroma_subsampling_y) << 2 | (seq_params.chroma_sample_position);
	av1header[3] = 0;

	out.insert(out.end(), av1header, av1header + 4);
	if (seq_size)
		out.insert(out.end(), seq, seq + seq_size);
	if (meta_size)
		out.insert(out.end(), meta, meta + meta_size);
	return true;
}

void ConvertAv1TemporalUnit(const uint8_t *data, size_t size, bool *keyframe, int *priority,
			    std::vector<uint8_t> &out)
{
	out.clear();
	serialize_av1_data(data, size, keyframe, priority, out);
}

} // namespace Srt
