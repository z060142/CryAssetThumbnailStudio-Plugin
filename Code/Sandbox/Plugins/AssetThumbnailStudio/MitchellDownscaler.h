// Copyright 2026
#pragma once

#include <QString>

namespace AssetThumbnailStudio
{

bool DownscaleMitchell(const QString& sourcePath, const QString& destinationPath, int destinationSize);
bool DownscaleQtSmoothReference(const QString& sourcePath, const QString& destinationPath, int destinationSize);
bool RunMitchellCheckerboardSelfTest();

} // namespace AssetThumbnailStudio
