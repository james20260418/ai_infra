#include "tools/opencv_util/opencv_util.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "glog/logging.h"
#include "gtest/gtest.h"

using namespace geom;

class OpencvUtilTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string cmd = "mkdir -p " + std::string(kTestOutputPath);
        system(cmd.c_str());
    }

    static constexpr char kTestOutputPath[] = "/james/ai_infra/output/";
};

TEST_F(OpencvUtilTest, BasicDrawing) {
    opencv_util::Figure f1({0.0, 0.0}, {19.5, 10.0});
    f1.DrawGrid(
        1.0,
        Vec2d{0.5, 0.1},
        opencv_util::Color(0.5, 0.5, 0.5, 1.0),
        2,
        opencv_util::Color(0.3, 0.1, 0.1, 1.0)
    );
    f1.FillRect({-1.0, -1.0}, {5.0, 4.0});
    f1.DrawLine({-1.0, -3.0}, {25.0, 4.0});

    opencv_util::Figure f2({0.0, 0.0}, {10.0, 10.0});
    f2.DrawGrid(
        1.0,
        Vec2d{0.5, 0.1},
        opencv_util::Color(0.5, 0.5, 0.5, 1.0),
        3,
        opencv_util::Color(0.3, 0.1, 0.1, 1.0)
    );
    f1.FillFigure(f2, {-3.0, 3.0}, {8.0, 8.0});

    std::string img_path(kTestOutputPath);
    EXPECT_TRUE(f1.SaveAsPng(img_path + "test_basic.png"));

    opencv_util::Figure f3({0.0, 0.0}, {4.0, 1.0});
    f3.FillRect({0.0, 0.0}, {1.0, 1.0}, opencv_util::Color(0.6, 0.0, 0.0));
    f3.FillRect({2.0, 0.0}, {3.0, 1.0}, opencv_util::Color(0.6, 0.0, 0.0));
    f3.DrawText({0.0, 0.0}, {4.0, 1.0}, "LongTextInSingleLine");
    f3.DrawText({0.0, 0.0}, {4.0, 1.0}, "Short");
    EXPECT_TRUE(f3.SaveAsPng(img_path + "test_text.png"));

    LOG(INFO) << "Basic drawing test passed, output saved to " << img_path;
}

TEST_F(OpencvUtilTest, VideoGeneration) {
    std::vector<opencv_util::Figure> figures;
    Vec2d middle(8.0, 4.5);
    double fps = 60.0;

    for (int i = 0; i < 180; ++i) {
        opencv_util::Figure f({0.0, 0.0}, {16.0, 9.0});
        f.DrawGrid(
            1.0,
            Vec2d{0.0, 0.0},
            opencv_util::Color(0.5, 0.5, 0.5, 1.0)
        );

        const double angle = (i / fps) * 1.0;
        f.DrawLine(middle, middle + Vec2d::UnitFromAngle(angle) * 3.0);

        figures.push_back(f);
    }

    bool result = opencv_util::MakeVideo(
        figures,
        fps,
        std::string(kTestOutputPath) + "test_output.avi"
    );
    EXPECT_TRUE(result);
    LOG(INFO) << "Video generation test passed";
}

TEST_F(OpencvUtilTest, EmptyInputReturnsError) {
    std::vector<opencv_util::Figure> empty_figures;
    EXPECT_DEATH(
        opencv_util::MakeVideo(empty_figures, 30.0, "should_not_be_created.avi"),
        ""
    );
}
