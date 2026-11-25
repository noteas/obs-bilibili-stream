#include "core/qr_generator.hpp"
#include "qrcodegen/qrcodegen.hpp"
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
					out << "M" << (x + border) * scale << " " << (y + border) * scale << "h" << scale
					    << "v" << scale << "h-" << scale << "z";
				}
			}
		}
		out << "\"/>\n</svg>\n";
		svg = out.str();
	} catch (const std::exception &) {
		return false;
	}

	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file.is_open())
		return false;
	file << svg;
	return file.good();
}

} // namespace Core
