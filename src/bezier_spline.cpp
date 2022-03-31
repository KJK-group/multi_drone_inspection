#include "multi_drone_inspection/bezier_spline.hpp"

#include <iostream>

namespace mdi {
//--------------------------------------------------------------------------------------------------
// Constructor; generates the spline the first time,
// calling generate_spline with `points` and `resolution`
BezierSpline::BezierSpline(vector<Vector3f> points, int resolution) {
    // generate the spline points
    generate_spline(points, resolution);
}

//--------------------------------------------------------------------------------------------------
// Generates the spline from the input points given in the vector `points`,
// at a time resolution of `resolution`
auto BezierSpline::generate_spline(vector<Vector3f> points, int resolution) -> void {
    this->input_points = points;
    this->resolution = resolution;
    this->size = points.size();
    // in case the binomial lookup table isn't big enough, generate a new one
    if (binomial_lut.size() != this->size) {
        this->generate_binomial_lut();
    }
    assert(points.size() == binomial_lut.size());
    this->generate_distance_lut();

    // let t run through [0;resolution] with steps defined by the resolution
    for (int t = 0; t <= resolution; t += 1) {
        auto time = (float)t / (float)resolution;
        spline_points.push_back(f(time));
    }
}

//--------------------------------------------------------------------------------------------------
// Returns the point along the spline at time `t`
// Uses the binomial LUT to perform these calculation fast
auto BezierSpline::f(float t) -> Vector3f {
    // sum up all bezier terms, using the explicit definition given by:
    // https://en.wikipedia.org/wiki/B%C3%A9zier_curve
    auto spline_point = Vector3f(0, 0, 0);
    for (int i = 0; i < this->size; i++) {
        auto w = binomial_lut[i] * pow(t, i) * pow(1 - t, this->size - i);
        spline_point += w * this->input_points[i];
    }
    return spline_point;
}

//--------------------------------------------------------------------------------------------------
// Approximates the arc length of the spline from the sum of euclidean distances
// between all the calculated points along the spline
auto BezierSpline::generate_distance_lut() -> void {
    // Approximate the arc length of the spline
    auto arc_length_sum = 0;
    // first entry into distance LUT at time 0
    this->distance_lut.push_back(arc_length_sum);
    for (int t = 1; t < spline_points.size(); t++) {
        // difference vector between every two points
        auto diff = spline_points[t - 1] - spline_points[t];
        // euclidean norm of the difference vector
        arc_length_sum += diff.norm();
        // cumulative distance entry for ever time step
        this->distance_lut.push_back(arc_length_sum);
    }
}

//--------------------------------------------------------------------------------------------------
// Generates the binomial LUT for a Bezier pline with `n` points
// This will be used to generate the spline
auto BezierSpline::generate_binomial_lut() -> void {
    for (int i = 0; i < this->size; i++) {
        binomial_lut.push_back(mdi::utils::binomial_coefficient(this->size, i));
    }
}

//--------------------------------------------------------------------------------------------------
// Returns the closest point at the given `time`, where 0<=time<=1.
auto BezierSpline::get_point_at_time(float time) -> Vector3f {
    assert(time <= 1.0 && time >= 0);
    return this->spline_points[(int)(resolution * time)];
}

//--------------------------------------------------------------------------------------------------
// Returns the point at the `distance` along the spline
auto BezierSpline::get_point_at_distance(float distance) -> Vector3f {
    auto t_idx = this->get_time_idx(distance);

    // given that the found time idx is the last idx,
    // the given distance must have been >= arc length,
    // thus the last point is returned
    if (t_idx == this->resolution) {
        return this->spline_points[t_idx];
    }
    // otherwise we want to interpolate between two time values,
    // the one given, which is the closest before the `distance`,
    // and the next one after the given `distance`

    // distance range to map from
    auto distance_diff = this->distance_lut[t_idx + 1] - this->distance_lut[t_idx];
    // distance to map
    auto distance_diff_given = this->distance_lut[t_idx + 1] - distance;
    // mapping to time with fraction of the above two
    auto diff_frac = distance_diff_given / distance_diff;
    assert(diff_frac >= 0 && diff_frac <= 1);
    // normalise to [0;1]
    auto t = ((float)t_idx + diff_frac) / resolution;

    return this->f(t);
}

//--------------------------------------------------------------------------------------------------
// Returns the closes time idx, the given `distance` comes after
// If `distance`<0 return minimum time idx; 0
// If `distance`>arc_length, return max time idx; resolution
auto BezierSpline::get_time_idx(float distance) -> float {
    auto found_t_idx = 0;
    for (int t_idx = 0; t_idx < this->resolution; ++t_idx) {
        if (this->distance_lut[t_idx] > distance) {
            if (abs(distance_lut[t_idx] - distance) < abs(distance_lut[t_idx - 1] - distance)) {
                found_t_idx = t_idx;
            } else {
                found_t_idx = t_idx - 1;
            }
            break;
        }
        found_t_idx = t_idx;
    }
    return found_t_idx;
}

//--------------------------------------------------------------------------------------------------
// Returns a vector of all points along the spline
auto BezierSpline::get_spline_points() -> vector<Vector3f> { return this->spline_points; }
}  // namespace mdi