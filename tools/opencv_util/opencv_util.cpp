#include "tools/opencv_util/opencv_util.h"

#include <filesystem>

#include "glog/logging.h"
#include "geom/common/common.h"
#include "geom/common/math_util.h"

namespace opencv_util {

    Figure::Figure(
        const geom::Vec2d& left_bottom,
        const geom::Vec2d& right_top,
        int width,
        int height,
        const Color& bgd_color
    )
        : mat_(/*row=*/height, /*col=*/width, CV_8UC4, bgd_color.To8UC4Color()),
          x0_(left_bottom.x()),
          y0_(left_bottom.y()),
          x_span_(right_top.x() - x0_),
          y_span_(right_top.y() - y0_)
    {
        CHECK(std::isfinite(x0_));
        CHECK(std::isfinite(y0_));
        CHECK_GT(x_span_, 0.0);
        CHECK_GT(y_span_, 0.0);
        CHECK_GT(width, 0);
        CHECK_GT(height, 0);

        const double xy_aa = x_span_ / y_span_;
        const double img_aa = (1.0 * width) / height;

        int actual_height = height;
        int actual_width = width;

        if (img_aa > xy_aa) {
            actual_width = std::clamp(
                static_cast<int>(xy_aa * height + 0.5), 1, width
            );
            origin_offset_.x = (width - actual_width) / 2;
            origin_offset_.y = 0;
            pixel_size_inv_ = height / y_span_;
        } else {
            actual_height = std::clamp(
                static_cast<int>(width / xy_aa + 0.5), 1, height
            );
            origin_offset_.y = (height - actual_height) / 2;
            origin_offset_.x = 0;
            pixel_size_inv_ = width / x_span_;
        }

        actual_drawing_region_ = cv::Rect(
            origin_offset_,
            origin_offset_ + cv::Point(actual_width, actual_height)
        );

        CHECK_GE(actual_drawing_region_.tl().x, 0);
        CHECK_GE(actual_drawing_region_.tl().y, 0);
        CHECK_LE(actual_drawing_region_.br().x, mat_.cols);
        CHECK_LE(actual_drawing_region_.br().y, mat_.rows);
    }

    bool Figure::SaveAsPng(const std::string& abs_path, bool create_directory_when_absent) const {
        if (create_directory_when_absent) {
            size_t slash_pos = abs_path.find_last_of("/");
            if (slash_pos != std::string::npos) {
                std::string folder_path = abs_path.substr(0, slash_pos);
                if (!std::filesystem::exists(folder_path)) {
                    std::filesystem::create_directories(folder_path);
                }
            }
        }

        std::vector<int> compression_params;
        compression_params.push_back(cv::IMWRITE_PNG_COMPRESSION);
        compression_params.push_back(9);
        return cv::imwrite(abs_path, mat_, compression_params);
    }

    void Figure::DrawLine(
        const geom::Vec2d& p1,
        const geom::Vec2d& p2,
        const Color& color,
        double width
    ) {
        cv::Point pt_1 = XyToPt(p1);
        cv::Point pt_2 = XyToPt(p2);
        if (!cv::clipLine(actual_drawing_region_, pt_1, pt_2)) {
            return;
        }
        cv::line(mat_, pt_1, pt_2, color.To8UC4Color(), GetThickness(width), CV_AA);
    }

    void Figure::DrawArc(
        const geom::Vec2d& circle_center,
        double radius,
        double width,
        double start_angle,
        double end_angle,
        int num_of_samples,
        const Color& color
    ) {
        CHECK_GE(radius, 0.0);
        CHECK_GT(num_of_samples, 1);

        const double angle_span = geom::WrapAngle(end_angle - start_angle);
        const double angle_step = angle_span / (num_of_samples - 1);

        std::vector<geom::Vec2d> points;
        points.reserve(num_of_samples);
        for (int i = 0; i < num_of_samples; ++i) {
            double angle = start_angle + i * angle_step;
            points.push_back(circle_center + geom::Vec2d::UnitFromAngle(angle) * radius);
        }

        for (int i = 1; i < static_cast<int>(points.size()); ++i) {
            DrawLine(points[i - 1], points[i], color, width);
        }
    }

    void Figure::FillRect(
        const geom::Vec2d& p1,
        const geom::Vec2d& p2,
        const Color& color
    ) {
        cv::rectangle(mat_, GetClipedRect(p1, p2), color.To8UC4Color(), CV_FILLED);
    }

    void Figure::FillMat(
        const cv::Mat& source_mat,
        const geom::Vec2d& p1,
        const geom::Vec2d& p2
    ) {
        const cv::Mat dst_sub_mat = sub_mat(p1, p2);
        cv::resize(source_mat, dst_sub_mat, cv::Size(dst_sub_mat.size()));
    }

    bool Figure::DrawGrid(
        double grid_size,
        const geom::Vec2d& origin,
        const Color& color,
        int grid_per_label,
        const Color& label_color
    ) {
        CHECK_GT(grid_size, 0.0);

        constexpr int kMarginalStep = 2;
        constexpr double kLineThicknessOccupancy = 0.02;
        constexpr double kOriginLineThicknessOccupancy = 0.04;
        constexpr double kTextThicknessOccupancy = 0.02;
        constexpr double kFontScaleFactor = 0.005;
        const cv::Point kTextOffset(1, -1);

        if (x_span_ / grid_size > kMaxNumGridPerDim ||
            y_span_ / grid_size > kMaxNumGridPerDim) {
            return false;
        }

        const int xid_max = static_cast<int>(
            (x0_ + x_span_ - origin.x()) / grid_size
        ) + kMarginalStep;
        const int xid_min = static_cast<int>(
            (x0_ - origin.x()) / grid_size
        ) - kMarginalStep;
        const int yid_max = static_cast<int>(
            (y0_ + y_span_ - origin.y()) / grid_size
        ) + kMarginalStep;
        const int yid_min = static_cast<int>(
            (y0_ - origin.y()) / grid_size
        ) - kMarginalStep;

        // Draw grid lines.
        for (int xid = xid_min; xid < xid_max; ++xid) {
            const double x = origin.x() + xid * grid_size;
            DrawLine(
                geom::Vec2d{ x, y0_ },
                geom::Vec2d{ x, y0_ + y_span_ },
                color,
                grid_size * (xid == 0 ? kOriginLineThicknessOccupancy : kLineThicknessOccupancy)
            );
        }
        for (int yid = yid_min; yid < yid_max; ++yid) {
            const double y = origin.y() + yid * grid_size;
            DrawLine(
                geom::Vec2d{ x0_, y },
                geom::Vec2d{ x0_ + x_span_, y },
                color,
                grid_size * (yid == 0 ? kOriginLineThicknessOccupancy : kLineThicknessOccupancy)
            );
        }

        // Label axis.
        if (grid_per_label > 0) {
            const double font_scale = kFontScaleFactor * grid_size *
                pixel_size_inv_ * std::min(grid_per_label, 3);
            const int text_thickness = GetThickness(kTextThicknessOccupancy * grid_size);

            for (int xid = xid_min; xid < xid_max; ++xid) {
                const double x = origin.x() + xid * grid_size;
                if (xid % grid_per_label == 0 &&
                    x >= x0_ && x <= x0_ + x_span_) {
                    cv::putText(
                        mat_,
                        geom::StrFormat("%.2f", x),
                        XyToPt(geom::Vec2d{ x, y0_ }) + kTextOffset,
                        cv::FONT_HERSHEY_SCRIPT_SIMPLEX,
                        font_scale,
                        label_color.To8UC4Color(),
                        text_thickness,
                        CV_AA,
                        false
                    );
                }
            }
            for (int yid = yid_min; yid < yid_max; ++yid) {
                const double y = origin.y() + yid * grid_size;
                if (yid % grid_per_label == 0 &&
                    y >= y0_ && y <= y0_ + y_span_) {
                    cv::putText(
                        mat_,
                        geom::StrFormat("%.2f", y),
                        XyToPt(geom::Vec2d{ x0_, y }) + kTextOffset,
                        cv::FONT_HERSHEY_SCRIPT_SIMPLEX,
                        font_scale,
                        label_color.To8UC4Color(),
                        text_thickness,
                        CV_AA,
                        false
                    );
                }
            }
        }

        return true;
    }

    void Figure::DrawText(
        const geom::Vec2d& p1,
        const geom::Vec2d& p2,
        const std::string& text,
        const Color& color,
        int thickness_ratio
    ) {
        const double x_min = std::min(p1.x(), p2.x());
        const double x_max = std::max(p1.x(), p2.x());
        const double y_min = std::min(p1.y(), p2.y());
        const double y_max = std::max(p1.y(), p2.y());

        if (x_max < x0_ || x_min > x0_ + x_span_) return;
        if (y_max < y0_ || y_min > y0_ + y_span_) return;

        const int num = static_cast<int>(text.size());
        if (num < 1) return;

        const double char_height = std::min(
            2.0 * (x_max - x_min) / num, y_max - y_min
        );

        const double font_scale = pixel_size_inv_ * 0.05 * char_height;
        const double thickness = thickness_ratio * GetThickness(0.01 * char_height);

        cv::putText(
            mat_,
            text,
            XyToPt(geom::Vec2d{ x_min, y_min + 0.1 * char_height }),
            cv::FONT_HERSHEY_PLAIN,
            font_scale,
            color.To8UC4Color(),
            thickness,
            CV_AA,
            false
        );
    }

    void Figure::ShowAndWait(const std::string& window_name) {
        cv::namedWindow(window_name);
        cv::imshow(window_name, mat_);
        cv::waitKey(0);
    }

    cv::Point Figure::XyToPt(const geom::Vec2d& xy) const {
        const int xid = cv::saturate_cast<int>(
            std::round((xy.x() - x0_) * pixel_size_inv_)
        );
        const int yid = cv::saturate_cast<int>(
            std::round((xy.y() - y0_) * pixel_size_inv_)
        );

        cv::Point offseted_pt = origin_offset_ + cv::Point{ /*col=*/xid, /*row=*/yid };
        offseted_pt.y = mat_.rows - 1 - offseted_pt.y;
        return offseted_pt;
    }

    cv::Rect Figure::GetClipedRect(const geom::Vec2d& p1, const geom::Vec2d& p2) const {
        cv::Rect raw_rect(XyToPt(p1), XyToPt(p2));

        const int x_min = std::max(
            actual_drawing_region_.tl().x, raw_rect.tl().x
        );
        const int x_max = std::min(
            actual_drawing_region_.br().x, raw_rect.br().x
        );
        const int y_min = std::max(
            actual_drawing_region_.tl().y, raw_rect.tl().y
        );
        const int y_max = std::min(
            actual_drawing_region_.br().y, raw_rect.br().y
        );

        return cv::Rect(cv::Point{ x_min, y_min }, cv::Point{ x_max, y_max });
    }

    bool MakeVideo(
        const std::vector<Figure>& figures,
        double fps,
        const std::string& filename,
        bool create_directory_when_absent
    ) {
        CHECK(!figures.empty());
        const cv::Mat& ref_mat = figures.front().mat();
        const cv::Size frame_size(ref_mat.cols, ref_mat.rows);

        if (create_directory_when_absent) {
            size_t slash_pos = filename.find_last_of("/");
            if (slash_pos != std::string::npos) {
                std::string folder_path = filename.substr(0, slash_pos);
                if (!std::filesystem::exists(folder_path)) {
                    std::filesystem::create_directories(folder_path);
                }
            }
        }

        cv::VideoWriter writer;
        writer.open(
            filename,
            cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
            fps,
            frame_size
        );

        if (!writer.isOpened()) {
            return false;
        }

        for (const auto& figure : figures) {
            CHECK_EQ(figure.mat().type(), CV_8UC4);

            cv::Mat buffer;
            cv::cvtColor(figure.mat(), buffer, cv::COLOR_RGBA2RGB);
            writer.write(buffer);
        }

        writer.release();
        return true;
    }

}  // namespace opencv_util
