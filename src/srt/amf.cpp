#include "srt/amf.hpp"

#include <cstring>

namespace Srt::Amf {

namespace {

void WriteU16(std::vector<uint8_t> &out, uint16_t v)
{
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v));
}

void WriteU32(std::vector<uint8_t> &out, uint32_t v)
{
	out.push_back(static_cast<uint8_t>(v >> 24));
	out.push_back(static_cast<uint8_t>(v >> 16));
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v));
}

double ReadDouble(const uint8_t *p)
{
	uint64_t bits = 0;
	for (int i = 0; i < 8; ++i)
		bits = (bits << 8) | p[i];
	double d = 0;
	std::memcpy(&d, &bits, sizeof(d));
	return d;
}

struct Reader {
	const uint8_t *p = nullptr;
	size_t n = 0;
	size_t off = 0;

	bool Has(size_t k) const { return off + k <= n; }
	uint8_t U8()
	{
		if (!Has(1))
			return 0;
		return p[off++];
	}
	uint16_t U16()
	{
		if (!Has(2))
			return 0;
		const uint16_t v = static_cast<uint16_t>((p[off] << 8) | p[off + 1]);
		off += 2;
		return v;
	}
	uint32_t U32()
	{
		if (!Has(4))
			return 0;
		const uint32_t v = (uint32_t(p[off]) << 24) | (uint32_t(p[off + 1]) << 16) |
				   (uint32_t(p[off + 2]) << 8) | uint32_t(p[off + 3]);
		off += 4;
		return v;
	}
	double Double()
	{
		if (!Has(8))
			return 0;
		const double d = ReadDouble(p + off);
		off += 8;
		return d;
	}
};

bool ReadString(Reader &r, std::string &out)
{
	const uint16_t len = r.U16();
	if (!r.Has(len))
		return false;
	out.assign(reinterpret_cast<const char *>(r.p + r.off), len);
	r.off += len;
	return true;
}

bool SkipValue(Reader &r, int depth);

// 读一个 object/ECMA array 的键值对，提取 code/level 字符串。
bool ReadObjectProps(Reader &r, int depth, std::string &code, std::string &level)
{
	while (r.Has(3)) {
		const uint16_t key_len = r.U16();
		if (key_len == 0 && r.Has(1) && r.p[r.off] == 0x09) {
			r.off += 1; // 00 00 09 = object end
			return true;
		}
		if (!r.Has(key_len))
			return false;
		std::string key(reinterpret_cast<const char *>(r.p + r.off), key_len);
		r.off += key_len;
		if (!r.Has(1))
			return false;
		const uint8_t type = r.U8();
		if (type == 0x02) {
			std::string value;
			if (!ReadString(r, value))
				return false;
			if (key == "code")
				code = value;
			else if (key == "level")
				level = value;
		} else {
			r.off -= 1;
			if (!SkipValue(r, depth + 1))
				return false;
		}
	}
	return false;
}

bool SkipValue(Reader &r, int depth)
{
	if (depth > 8 || !r.Has(1))
		return false;
	switch (r.p[r.off++]) {
	case 0x00: // number
		if (!r.Has(8))
			return false;
		r.off += 8;
		return true;
	case 0x01: // boolean
		if (!r.Has(1))
			return false;
		r.off += 1;
		return true;
	case 0x02: { // string
		std::string tmp;
		return ReadString(r, tmp);
	}
	case 0x03: { // object
		std::string c, l;
		return ReadObjectProps(r, depth, c, l);
	}
	case 0x05: // null
	case 0x06: // undefined
		return true;
	case 0x08: { // ECMA array
		r.off += 4;
		std::string c, l;
		return ReadObjectProps(r, depth, c, l);
	}
	case 0x0A: { // strict array
		const uint32_t count = r.U32();
		for (uint32_t i = 0; i < count; ++i) {
			if (!SkipValue(r, depth + 1))
				return false;
		}
		return true;
	}
	case 0x0B: // date
		if (!r.Has(10))
			return false;
		r.off += 10;
		return true;
	default:
		return false;
	}
}

} // namespace

void WriteNumber(std::vector<uint8_t> &out, double value)
{
	out.push_back(0x00);
	uint64_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	for (int i = 7; i >= 0; --i)
		out.push_back(static_cast<uint8_t>(bits >> (i * 8)));
}

void WriteBool(std::vector<uint8_t> &out, bool value)
{
	out.push_back(0x01);
	out.push_back(value ? 0x01 : 0x00);
}

void WriteString(std::vector<uint8_t> &out, const std::string &value)
{
	out.push_back(0x02);
	const size_t len = value.size() > 0xFFFF ? 0xFFFF : value.size();
	WriteU16(out, static_cast<uint16_t>(len));
	out.insert(out.end(), value.begin(), value.begin() + static_cast<long>(len));
}

void WriteNull(std::vector<uint8_t> &out)
{
	out.push_back(0x05);
}

void WriteKey(std::vector<uint8_t> &out, const std::string &key)
{
	WriteU16(out, static_cast<uint16_t>(key.size()));
	out.insert(out.end(), key.begin(), key.end());
}

void BeginObject(std::vector<uint8_t> &out)
{
	out.push_back(0x03);
}

void EndObject(std::vector<uint8_t> &out)
{
	out.push_back(0x00);
	out.push_back(0x00);
	out.push_back(0x09);
}

void BeginEcmaArray(std::vector<uint8_t> &out, uint32_t count)
{
	out.push_back(0x08);
	WriteU32(out, count);
}

bool DecodeCommand(const uint8_t *data, size_t size, CommandInfo &out)
{
	out = CommandInfo{};
	Reader r{data, size, 0};
	if (!r.Has(1) || r.p[r.off] != 0x02)
		return false;
	r.off += 1;
	if (!ReadString(r, out.name))
		return false;

	if (r.Has(1) && r.p[r.off] == 0x00) {
		r.off += 1;
		out.transaction = r.Double();
	}

	while (r.Has(1)) {
		const uint8_t type = r.p[r.off];
		if (type == 0x03) {
			r.off += 1;
			if (!ReadObjectProps(r, 0, out.code, out.level))
				break;
		} else if (type == 0x08) {
			r.off += 5; // marker + count
			if (!ReadObjectProps(r, 0, out.code, out.level))
				break;
		} else if (type == 0x00) {
			r.off += 1;
			out.last_number = r.Double();
			out.has_last_number = true;
		} else {
			if (!SkipValue(r, 0))
				break;
		}
	}
	return true;
}

} // namespace Srt::Amf
