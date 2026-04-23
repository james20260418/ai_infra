#pragma once

#include <optional>

#include "glog/logging.h"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/videoio.hpp"

#include "geom/common/vec.h"

namespace opencv_util {

    struct Color {
        static constexpr double kUcharRangeMax = 255.0;

        // 0.0 ~ 1.0
        static constexpr Color RGBA(double r, double g, double b, double a) {
            return Color{ .r = r, .g = g, .b = b, .a = a };
        }

        // 0.0 ~ 1.0, Alpha is 1.0.
        static constexpr Color RGB(double r, double g, double b) {
            return Color::RGBA(r, g, b, 1.0);
        }

        cv::Vec4b ToCvVec4() const {
            cv::Vec4b bgra;
            bgra[0] = cv::saturate_cast<uchar>(b * UCHAR_MAX);
            bgra[1] = cv::saturate_cast<uchar>(g * UCHAR_MAX);
            bgra[2] = cv::saturate_cast<uchar>(r * UCHAR_MAX);
            bgra[3] = cv::saturate_cast<uchar>(a * UCHAR_MAX);
            return bgra;
        }

        cv::Scalar To8UC4Color() const {
            return cv::Scalar(
                b * kUcharRangeMax,
                g * kUcharRangeMax,
                r * kUcharRangeMax,
                a * kUcharRangeMax
            );
        }

        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        double a = 0.0;
    };

    class Figure {
    public:
        static constexpr int kDefaultWidth = 1280;
        static constexpr int kDefaultHeight = 720;
        static constexpr Color kDefaultBgd{ 1.0, 1.0, 1.0, 1.0 };
        static constexpr Color kDefaultColor{ 0.0, 0.0, 0.0, 1.0 };
        static constexpr int kMaxNumGridPerDim = 1000;

        Figure(
            const geom::Vec2d& left_bottom,
            const geom::Vec2d& right_top,
            int width = kDefaultWidth,
            int height = kDefaultHeight,
            const Color& bgd_color = kDefaultBgd
        );

        bool SaveAsPng(const std::string& abs_path, bool create_directory_when_absent = true) const;

        void DrawLine(
            const geom::Vec2d& p1,
            const geom::Vec2d& p2,
            const Color& color = kDefaultColor,
            double width = 0.0
        );

        void DrawArc(
            const geom::Vec2d& circle_center,
            double radius,
            double width,
            double start_angle,
            double end_angle,
            int num_of_samples,
            const Color& color = kDefaultColor
        );

        void FillRect(
            const geom::Vec2d& p1,
            const geom::Vec2d& p2,
            const Color& color = kDefaultColor
        );

        void FillMat(const cv::Mat& mat, const geom::Vec2d& p1, const geom::Vec2d& p2);

        void FillFigure(const Figure& other, const geom::Vec2d& p1, const geom::Vec2d& p2) {
            FillMat(other.mat(), p1, p2);
        }

        bool DrawGrid(
            double grid_size = 1.0,
            const geom::Vec2d& origin = geom::Vec2d(0.0, 0.0),
            const Color& color = kDefaultColor,
            int grid_per_label = -1,
            const Color& label_color = kDefaultColor
        );

        void DrawText(
            const geom::Vec2d& p1,
            const geom::Vec2d& p2,
            const std::string& text,
            const Color& color = kDefaultColor,
            int thickness_ratio = 1
        );

        void ShowAndWait(const std::string& window_name);

        int width() const { return mat_.rows; }
        int height() const { return mat_.cols; }

        const cv::Mat& mat() const { return mat_; }

        cv::Mat actual_drawing_region_mat() const {
            return mat_(actual_drawing_region_);
        }

        cv::Mat sub_mat(const geom::Vec2d& p1, const geom::Vec2d& p2) const {
            return mat_(GetClipedRect(p1, p2));
        }

        Figure(const Figure& other)
            : mat_(other.mat_.clone()),
              x0_(other.x0_),
              y0_(other.y0_),
              x_span_(other.x_span_),
              y_span_(other.y_span_),
              actual_drawing_region_(other.actual_drawing_region_),
              origin_offset_(other.origin_offset_),
              pixel_size_inv_(other.pixel_size_inv_) {}

    private:
        cv::Point XyToPt(const geom::Vec2d& xy) const;
        cv::Rect GetClipedRect(const geom::Vec2d& p1, const geom::Vec2d& p2) const;

        int GetThickness(double thickness_in_xy) const {
            double raw_pixel_num = thickness_in_xy * pixel_size_inv_;
            return std::max(1, static_cast<int>(raw_pixel_num + 0.5));
        }

        cv::Mat mat_;
        double x0_ = 0.0;
        double y0_ = 0.0;
        double x_span_ = 0.0;
        double y_span_ = 0.0;
        cv::Rect actual_drawing_region_;
        cv::Point origin_offset_;
        double pixel_size_inv_ = 0.0;
    };

    bool MakeVideo(
        const std::vector<Figure>& figures,
        double fps,
        const std::string& filename,
        bool create_directory_when_absent = true
    );

}  // namespace opencv_util
