#include "core/qr_generator.hpp"
#include "qrcodegen/qrcodegen.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Core {

bool QrGenerator::writeSvgFile(const std::string &path, const std::string &data)
{
	if (data.empty())
		return false;

	using qrcodegen::QrCode;
	std::string svg;
	try {
		const QrCode qr = QrCode::encodeText(data.c_str(), QrCode::Ecc::LOW);
		if (qr.getSize() <= 0)
			return false;

		const int scale = 8;
		const int border = 4;
		const int size = qr.getSize();
		const int full = (size + border * 2) * scale;

		std::ostringstream out;
		out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << full << "\" height=\"" << full
		    << "\" viewBox=\"0 0 " << full << " " << full << "\">\n"
		    << "<rect width=\"" << full << "\" height=\"" << full << "\" fill=\"#ffffff\"/>\n"
		    << "<path fill=\"#000000\" d=\"";
		for (int y = 0; y < size; ++y) {
			for (int x = 0; x < size; ++x) {
				if (qr.getModule(x, y)) {
					out << "M" << (x + border) * scale << " " << (y + border) * scale << "h"
					    << scale << "v" << scale << "h-" << scale << "z";
				}
			}
		}
		out << "\"/>\n</svg>\n";
		svg = out.str();
	} catch (const std::exception &) {
		return false;
	}

	// path 是 UTF-8(obs_module_config_path):MSVC 的窄串构造按 ANSI 代码页解码,
	// 中文用户名目录会打不开文件,必须经 u8path。
	std::ofstream file(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
	if (!file.is_open())
		return false;
	file << svg;
	return file.good();
}

} // namespace Core
