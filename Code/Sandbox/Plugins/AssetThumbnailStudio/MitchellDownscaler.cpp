// Copyright 2026
#include "StdAfx.h"
#include "MitchellDownscaler.h"

#include <QImage>

#include <algorithm>
#include <cmath>
#include <vector>

namespace AssetThumbnailStudio
{
namespace
{

struct SFloatPixel
{
	double r = 0.0;
	double g = 0.0;
	double b = 0.0;
	double a = 0.0;
};

double MitchellNetravali(double x)
{
	constexpr double b = 1.0 / 3.0;
	constexpr double c = 1.0 / 3.0;
	x = std::abs(x);

	if (x < 1.0)
	{
		return ((12.0 - 9.0 * b - 6.0 * c) * x * x * x
		      + (-18.0 + 12.0 * b + 6.0 * c) * x * x
		      + (6.0 - 2.0 * b)) / 6.0;
	}
	if (x < 2.0)
	{
		return ((-b - 6.0 * c) * x * x * x
		      + (6.0 * b + 30.0 * c) * x * x
		      + (-12.0 * b - 48.0 * c) * x
		      + (8.0 * b + 24.0 * c)) / 6.0;
	}
	return 0.0;
}

int ClampSourceIndex(int index, int extent)
{
	return std::max(0, std::min(index, extent - 1));
}

int ToByte(double value)
{
	return static_cast<int>(std::lround(std::max(0.0, std::min(value, 255.0))));
}

QImage ResampleMitchell(const QImage& input, int destinationWidth, int destinationHeight)
{
	if (input.isNull() || destinationWidth <= 0 || destinationHeight <= 0)
	{
		return QImage();
	}

	const QImage source = input.convertToFormat(QImage::Format_ARGB32);
	const double scaleX = static_cast<double>(source.width()) / destinationWidth;
	const double scaleY = static_cast<double>(source.height()) / destinationHeight;
	const double supportX = 2.0 * std::max(1.0, scaleX);
	const double supportY = 2.0 * std::max(1.0, scaleY);

	std::vector<SFloatPixel> horizontal(static_cast<size_t>(destinationWidth) * source.height());
	for (int y = 0; y < source.height(); ++y)
	{
		for (int destinationX = 0; destinationX < destinationWidth; ++destinationX)
		{
			const double sourceCenter = (destinationX + 0.5) * scaleX - 0.5;
			const int firstSourceX = static_cast<int>(std::ceil(sourceCenter - supportX));
			const int lastSourceX = static_cast<int>(std::floor(sourceCenter + supportX));
			SFloatPixel sum;
			double weightSum = 0.0;

			for (int sourceX = firstSourceX; sourceX <= lastSourceX; ++sourceX)
			{
				const double weight = MitchellNetravali((sourceCenter - sourceX) / std::max(1.0, scaleX));
				const QRgb pixel = source.pixel(ClampSourceIndex(sourceX, source.width()), y);
				sum.r += qRed(pixel) * weight;
				sum.g += qGreen(pixel) * weight;
				sum.b += qBlue(pixel) * weight;
				sum.a += qAlpha(pixel) * weight;
				weightSum += weight;
			}

			if (std::abs(weightSum) <= 1.0e-12)
			{
				return QImage();
			}
			SFloatPixel& output = horizontal[static_cast<size_t>(y) * destinationWidth + destinationX];
			output.r = sum.r / weightSum;
			output.g = sum.g / weightSum;
			output.b = sum.b / weightSum;
			output.a = sum.a / weightSum;
		}
	}

	QImage destination(destinationWidth, destinationHeight, QImage::Format_ARGB32);
	for (int destinationY = 0; destinationY < destinationHeight; ++destinationY)
	{
		const double sourceCenter = (destinationY + 0.5) * scaleY - 0.5;
		const int firstSourceY = static_cast<int>(std::ceil(sourceCenter - supportY));
		const int lastSourceY = static_cast<int>(std::floor(sourceCenter + supportY));
		QRgb* const outputLine = reinterpret_cast<QRgb*>(destination.scanLine(destinationY));

		for (int x = 0; x < destinationWidth; ++x)
		{
			SFloatPixel sum;
			double weightSum = 0.0;
			for (int sourceY = firstSourceY; sourceY <= lastSourceY; ++sourceY)
			{
				const double weight = MitchellNetravali((sourceCenter - sourceY) / std::max(1.0, scaleY));
				const SFloatPixel& pixel = horizontal[static_cast<size_t>(ClampSourceIndex(sourceY, source.height())) * destinationWidth + x];
				sum.r += pixel.r * weight;
				sum.g += pixel.g * weight;
				sum.b += pixel.b * weight;
				sum.a += pixel.a * weight;
				weightSum += weight;
			}

			if (std::abs(weightSum) <= 1.0e-12)
			{
				return QImage();
			}
			outputLine[x] = qRgba(
				ToByte(sum.r / weightSum),
				ToByte(sum.g / weightSum),
				ToByte(sum.b / weightSum),
				ToByte(sum.a / weightSum));
		}
	}

	return destination;
}

} // namespace

bool DownscaleMitchell(const QString& sourcePath, const QString& destinationPath, int destinationSize)
{
	const QImage source(sourcePath);
	const QImage destination = ResampleMitchell(source, destinationSize, destinationSize);
	return !destination.isNull() && destination.save(destinationPath, "PNG");
}

bool DownscaleQtSmoothReference(const QString& sourcePath, const QString& destinationPath, int destinationSize)
{
	const QImage source(sourcePath);
	if (source.isNull())
	{
		return false;
	}

	const QImage destination = source.scaled(
		destinationSize, destinationSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	return !destination.isNull() && destination.save(destinationPath, "PNG");
}

bool RunMitchellCheckerboardSelfTest()
{
	QImage checkerboard(8, 8, QImage::Format_ARGB32);
	for (int y = 0; y < checkerboard.height(); ++y)
	{
		for (int x = 0; x < checkerboard.width(); ++x)
		{
			const int value = ((x + y) & 1) == 0 ? 0 : 255;
			checkerboard.setPixel(x, y, qRgba(value, value, value, 255));
		}
	}

	const QImage result = ResampleMitchell(checkerboard, 4, 4);
	if (result.isNull())
	{
		return false;
	}

	for (int y = 0; y < result.height(); ++y)
	{
		for (int x = 0; x < result.width(); ++x)
		{
			const QRgb pixel = result.pixel(x, y);
			if (std::abs(qRed(pixel) - qGreen(pixel)) > 1
				|| std::abs(qGreen(pixel) - qBlue(pixel)) > 1
				|| qAlpha(pixel) != 255)
			{
				return false;
			}
		}
	}
	return true;
}

} // namespace AssetThumbnailStudio
