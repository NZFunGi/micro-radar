#pragma once

// Shared degrees<->km conversion, used everywhere the radar radius (stored
// and transmitted in degrees, since that's what Projection::ToScreen and
// every geometry calculation actually use) needs to be shown to a person.
// 111.32 km/degree is the standard km-per-degree-of-latitude constant;
// applied uniformly to both axes here since the projection already treats
// the configured radius symmetrically in lat and lon. Matches the same
// constant independently defined in MicroRadarCompanion's MainWindow.xaml.cs
// (can't share a header across the C#/C++ boundary, but the value must stay
// in sync with this one).
namespace GeoUnits {
    constexpr double KM_PER_DEGREE = 111.32;
}
