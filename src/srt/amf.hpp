#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Srt::Amf {

// 只实现 RTMP publish 需要的 AMF0 子集：number/bool/string/null/object/ecma-array。
void WriteNumber(std::vector<uint8_t> &out, double value);
void WriteBool(std::vector<uint8_t> &out, bool value);
void WriteString(std::vector<uint8_t> &out, const std::string &value);
void WriteNull(std::vector<uint8_t> &out);
void WriteKey(std::vector<uint8_t> &out, const std::string &key);
void BeginObject(std::vector<uint8_t> &out);
void EndObject(std::vector<uint8_t> &out);
void BeginEcmaArray(std::vector<uint8_t> &out, uint32_t count);

// RTMP 命令/状态解析结果。
struct CommandInfo {
	std::string name;        // connect / _result / onStatus / releaseStream …
	double transaction = 0;  // 命令事务号
	double last_number = 0;  // 顶层最后一个 number（createStream 的 stream id）
	bool has_last_number = false;
	std::string code;        // onStatus.code
	std::string level;       // onStatus.level
};

bool DecodeCommand(const uint8_t *data, size_t size, CommandInfo &out);

} // namespace Srt::Amf
