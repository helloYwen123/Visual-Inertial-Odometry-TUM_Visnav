/**
BSD 3-Clause License

Copyright (c) 2018, Vladyslav Usenko and Nikolaus Demmel.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <CLI/CLI.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <queue>
#include <sophus/se3.hpp>
#include <sstream>
#include <thread>
#include <sophus/se3.hpp>
#include <time.h>

#include <tbb/concurrent_unordered_map.h>
#include <pangolin/display/image_view.h>
#include <pangolin/display/default_font.h>
#include <pangolin/image/image.h>
#include <pangolin/image/image_io.h>
#include <pangolin/image/typed_image.h>
#include <pangolin/pangolin.h>



#include <visnav/common_types.h>

#include <visnav/calibration.h>

#include <visnav/keypoints.h>
#include <visnav/map_utils.h>
#include <visnav/matching_utils.h>
#include <visnav/vo_utils.h>

#include <visnav/gui_helper.h>
#include <visnav/tracks.h>

#include <visnav/serialization.h>
#include <visnav/imudata_load.h>
#include <visnav/preintegration_imu/imu_types.h>
#include <visnav/preintegration_imu/preintegration.h>
#include <visnav/preintegration_imu/calib_bias.hpp>
#include <visnav/preintegration_imu/utils/assert.h>
#include <visnav/preintegration_imu/utils/eigen_utils.hpp>
using namespace visnav;

///////////////////////////////////////////////////////////////////////////////
/// Declarations
///////////////////////////////////////////////////////////////////////////////

void draw_image_overlay(pangolin::View& v, size_t view_id);
void change_display_to_image(const FrameCamId& fcid);
void draw_scene();
void load_data(const std::string& path, const std::string& calib_path);
bool next_step();
void optimize();
void compute_projections();

///////////////////////////////////////////////////////////////////////////////
/// Declarations for IMU 
///////////////////////////////////////////////////////////////////////////////
// void load_imu(            // load imu data
//     DatasetIoInterfacePtr& dataset_io,
//     std::queue<std::pair<Timestamp, ImuData<double>::Ptr>>& imu_data_queue,  // <timestamp , data(timestamp acceleration gyro)>
//     std::vector<Timestamp>& timestamps_imu, 
//     CalibAccelBias<double>& calib_accel,
//     CalibGyroBias<double>& calib_gyro);
void saveTrajectoryButton();
double alignSVD(
    const std::vector<int64_t>& filter_t_ns,
    const std::vector<Eigen::Vector3d,
                      Eigen::aligned_allocator<Eigen::Vector3d>>& filter_t_w_i,
    const std::vector<int64_t>& gt_t_ns,
    std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d>>&
        gt_t_w_i);
double SVD_APPLY();

///////////////////////////////////////////////////////////////////////////////
/// Constants
///////////////////////////////////////////////////////////////////////////////

constexpr int UI_WIDTH = 200;
constexpr int NUM_CAMS = 2;

///////////////////////////////////////////////////////////////////////////////
/// Variables
///////////////////////////////////////////////////////////////////////////////

int current_frame = 0;
Sophus::SE3d current_pose;
bool take_keyframe = true;
TrackId next_landmark_id = 0;

std::atomic<bool> opt_running{false};
std::atomic<bool> opt_finished{false};

std::set<FrameId> kf_frames;

std::shared_ptr<std::thread> opt_thread;

///the num that is allowed to exist in the map (default size: 11)
size_t num_latest_frames = 11; 
std::vector<FrameCamId> removed_fcid_buffer;

/// intrinsic calibration
Calibration calib_cam;
Calibration calib_cam_opt;
CalibAccelBias<double> calib_acc;
CalibGyroBias<double> calib_gyro;

// initialization for imu
bool imu = false;
bool initialized = false;
using Vec3 = Eigen::Matrix<double, 3, 1>;
Eigen::aligned_map<Timestamp, IntegratedImuMeasurement<double>> imu_measurements;
IntegratedImuMeasurement<double>::Ptr imu_measurement;
ImuData<double>::Ptr imudata;
Sophus::SE3d T_w_i_init;
Vec3 vel_w_i_init;
PoseVelState<double> first_state;  // initial state for imu
// the last state's timestamp
Timestamp last_state_t_ns;

std::queue<std::pair<Timestamp, ImuData<double>::Ptr>> imu_data_queue;
std::vector<Timestamp> imu_timestamps;

PoseVelState<double> frame_state;
Eigen::aligned_map<Timestamp, PoseVelState<double>> frame_states;
Eigen::aligned_map<Timestamp, PoseVelState<double>> frame_states_opt;
Eigen::aligned_map<Timestamp, PoseVelState<double>> frame_states_kf;


/// loaded images
tbb::concurrent_unordered_map<FrameCamId, std::string> images;

/// timestamps for all stereo pairs
std::vector<Timestamp> timestamps;

/// detected feature locations and descriptors
Corners feature_corners;

/// pairwise feature matches
Matches feature_matches;

/// camera poses in the current map
Cameras cameras;

/// copy of cameras for optimization in parallel thread
Cameras cameras_opt;

/// landmark positions and feature observations in current map
Landmarks landmarks;

/// copy of landmarks for optimization in parallel thread
Landmarks landmarks_opt;

/// landmark positions that were removed from the current map
Landmarks old_landmarks;

// initialize recent cameras for imu's state update 
Cameras recent_kf_cameras;

//Ground Truth timestamp and pose
std::vector<Timestamp> gt_t_ns;
std::vector<Sophus::SE3d, Eigen::aligned_allocator<Sophus::SE3d>>
    gt_t_w_i;
// record vio's Trajectory
std::vector<Sophus::SE3d, Eigen::aligned_allocator<Sophus::SE3d>>
    vio_t_w_i;

// record vio and gt's Trajectory for showing
std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d>>
    gt_points;

std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d>>
    vio_points;
    
// vio's timestamp
std::vector<Timestamp> vio_t_ns;

// old keyframe delete
Camera delete_camera;
FrameId delete_fid;

/// cashed info on reprojected landmarks; recomputed every time time from
/// cameras, landmarks, and feature_tracks; used for visualization and
/// determining outliers; indexed by images
ImageProjections image_projections;
std::string dataset_type = "euroc"; ///////////////
std::string imu_dataset_path =   ////////////
    "../data/euro_data/MH_01_easy";  // This should be set as a argument ...
                                     // will be changed later


///////////////////////////////////////////////////////////////////////////////
/// GUI parameters
///////////////////////////////////////////////////////////////////////////////

// The following GUI elements can be enabled / disabled from the main panel by
// switching the prefix from "ui" to "hidden" or vice verca. This way you can
// show only the elements you need / want for development.

pangolin::Var<bool> ui_show_hidden("ui.show_extra_options", false, true);

//////////////////////////////////////////////
/// Image display options

pangolin::Var<int> show_frame1("ui.show_frame1", 0, 0, 1500);
pangolin::Var<int> show_cam1("ui.show_cam1", 0, 0, NUM_CAMS - 1);
pangolin::Var<int> show_frame2("ui.show_frame2", 0, 0, 1500);
pangolin::Var<int> show_cam2("ui.show_cam2", 1, 0, NUM_CAMS - 1);
pangolin::Var<bool> lock_frames("ui.lock_frames", true, true);
pangolin::Var<bool> show_detected("ui.show_detected", true, true);
pangolin::Var<bool> show_matches("ui.show_matches", true, true);
pangolin::Var<bool> show_inliers("ui.show_inliers", true, true);
pangolin::Var<bool> show_reprojections("ui.show_reprojections", true, true);
pangolin::Var<bool> show_outlier_observations("ui.show_outlier_obs", false,
                                              true);
pangolin::Var<bool> show_ids("ui.show_ids", false, true);
pangolin::Var<bool> show_epipolar("hidden.show_epipolar", false, true);
pangolin::Var<bool> show_cameras3d("hidden.show_cameras", true, true);
pangolin::Var<bool> show_points3d("hidden.show_points", true, true);
pangolin::Var<bool> show_old_points3d("hidden.show_old_points3d", true, true);
pangolin::Var<bool> show_gt("hidden.show_gt", false, true);
pangolin::Var<bool> show_vio_pt("hidden.show_vio_pt", false, true);
//////////////////////////////////////////////
/// Feature extraction and matching options

pangolin::Var<int> num_features_per_image("hidden.num_features", 1500, 10,
                                          5000);
pangolin::Var<bool> rotate_features("hidden.rotate_features", true, true);
pangolin::Var<int> feature_match_max_dist("hidden.match_max_dist", 70, 1, 255);
pangolin::Var<double> feature_match_test_next_best("hidden.match_next_best",
                                                   1.2, 1, 4);

pangolin::Var<double> match_max_dist_2d("hidden.match_max_dist_2d", 20.0, 1.0,
                                        50);

pangolin::Var<int> new_kf_min_inliers("hidden.new_kf_min_inliers", 80, 1, 200);

pangolin::Var<int> max_num_kfs("hidden.max_num_kfs", 10, 5, 20);

pangolin::Var<double> cam_z_threshold("hidden.cam_z_threshold", 0.1, 1.0, 0.0);

//////////////////////////////////////////////
/// Adding cameras and landmarks options

pangolin::Var<double> reprojection_error_pnp_inlier_threshold_pixel(
    "hidden.pnp_inlier_thresh", 3.0, 0.1, 10);

//////////////////////////////////////////////
/// Bundle Adjustment Options("" , , , )
//prefix is hidden. others are ui
pangolin::Var<bool> ba_optimize_intrinsics("hidden.ba_opt_intrinsics", false,
                                           true);  
pangolin::Var<int> ba_verbose("hidden.ba_verbose", 1, 0, 2);

pangolin::Var<double> reprojection_error_huber_pixel("hidden.ba_huber_width",
                                                     1.0, 0.1, 10);

///////////////////////////////////////////////////////////////////////////////
/// GUI buttons
///////////////////////////////////////////////////////////////////////////////

// if you enable this, next_step is called repeatedly until completion
pangolin::Var<bool> continue_next("ui.continue_next", false, true);

using Button = pangolin::Var<std::function<void(void)>>;

Button next_step_btn("ui.next_step", &next_step);
Button save_traj_btn("ui.save_traj", &saveTrajectoryButton);
Button SVD_btn("ui.svd", &SVD_APPLY);


///////////////////////////////////////////////////////////////////////////////
/// GUI and Boilerplate Implementation
///////////////////////////////////////////////////////////////////////////////

// Parse parameters, load data, and create GUI window and event loop (or
// process everything in non-gui mode).
int main(int argc, char** argv) {
  bool show_gui = true;
  std::string dataset_path = "data/V1_01_easy/mav0";
  std::string cam_calib = "opt_calib.json";

  CLI::App app{"Visual odometry."};

  app.add_option("--show-gui", show_gui, "Show GUI");
  app.add_option("--dataset-path", dataset_path,
                 "Dataset path. Default: " + dataset_path);
  app.add_option("--cam-calib", cam_calib,
                 "Path to camera calibration. Default: " + cam_calib);
  app.add_option("--imu", imu, "VIO");
  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }

  load_data(dataset_path, cam_calib);

  

  if (show_gui) {
    pangolin::CreateWindowAndBind("Main", 1800, 1000);

    glEnable(GL_DEPTH_TEST);

    // main parent display for images and 3d viewer
    pangolin::View& main_view =
        pangolin::Display("main")
            .SetBounds(0.0, 1.0, pangolin::Attach::Pix(UI_WIDTH), 1.0)
            .SetLayout(pangolin::LayoutEqualVertical);

    // parent display for images
    pangolin::View& img_view_display =
        pangolin::Display("images").SetLayout(pangolin::LayoutEqual);
    main_view.AddDisplay(img_view_display);

    // main ui panel
    pangolin::CreatePanel("ui").SetBounds(0.0, 1.0, 0.0,
                                          pangolin::Attach::Pix(UI_WIDTH));

    // extra options panel
    pangolin::View& hidden_panel = pangolin::CreatePanel("hidden").SetBounds(
        0.0, 1.0, pangolin::Attach::Pix(UI_WIDTH),
        pangolin::Attach::Pix(2 * UI_WIDTH));
    ui_show_hidden.Meta().gui_changed = true;

    // 2D image views
    std::vector<std::shared_ptr<pangolin::ImageView>> img_view;
    while (img_view.size() < NUM_CAMS) {
      std::shared_ptr<pangolin::ImageView> iv(new pangolin::ImageView);

      size_t idx = img_view.size();
      img_view.push_back(iv);

      img_view_display.AddDisplay(*iv);
      iv->extern_draw_function =
          std::bind(&draw_image_overlay, std::placeholders::_1, idx);
    }

    // 3D visualization (initial camera view optimized to see full map)
    pangolin::OpenGlRenderState camera(
        pangolin::ProjectionMatrix(640, 480, 400, 400, 320, 240, 0.001, 10000),
        pangolin::ModelViewLookAt(-3.4, -3.7, -8.3, 2.1, 0.6, 0.2,
                                  pangolin::AxisNegY));

    pangolin::View& display3D =
        pangolin::Display("scene")
            .SetAspect(-640 / 480.0)
            .SetHandler(new pangolin::Handler3D(camera));
    main_view.AddDisplay(display3D);

    // Initialize variables to calculate total time for next_step to return true
    bool measuring_time = false;
    auto start = std::chrono::high_resolution_clock::time_point();
    auto end = std::chrono::high_resolution_clock::time_point();

    while (!pangolin::ShouldQuit()) {
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

      if (ui_show_hidden.GuiChanged()) {
        hidden_panel.Show(ui_show_hidden);
        const int panel_width = ui_show_hidden ? 2 * UI_WIDTH : UI_WIDTH;
        main_view.SetBounds(0.0, 1.0, pangolin::Attach::Pix(panel_width), 1.0);
      }

      display3D.Activate(camera);
      glClearColor(0.95f, 0.95f, 0.95f, 1.0f);  // light gray background

      draw_scene();

      img_view_display.Activate();

      if (lock_frames) {
        // in case of locking frames, chaning one should change the other
        if (show_frame1.GuiChanged()) {
          change_display_to_image(FrameCamId(show_frame1, 0));
          change_display_to_image(FrameCamId(show_frame1, 1));
        } else if (show_frame2.GuiChanged()) {
          change_display_to_image(FrameCamId(show_frame2, 0));
          change_display_to_image(FrameCamId(show_frame2, 1));
        }
      }

      if (show_frame1.GuiChanged() || show_cam1.GuiChanged()) {
        auto frame_id = static_cast<FrameId>(show_frame1);
        auto cam_id = static_cast<CamId>(show_cam1);

        FrameCamId fcid;
        fcid.frame_id = frame_id;
        fcid.cam_id = cam_id;
        if (images.find(fcid) != images.end()) {
          pangolin::TypedImage img = pangolin::LoadImage(images[fcid]);
          img_view[0]->SetImage(img);
        } else {
          img_view[0]->Clear();
        }
      }

      if (show_frame2.GuiChanged() || show_cam2.GuiChanged()) {
        auto frame_id = static_cast<FrameId>(show_frame2);
        auto cam_id = static_cast<CamId>(show_cam2);

        FrameCamId fcid;
        fcid.frame_id = frame_id;
        fcid.cam_id = cam_id;
        if (images.find(fcid) != images.end()) {
          pangolin::GlPixFormat fmt;
          fmt.glformat = GL_LUMINANCE;
          fmt.gltype = GL_UNSIGNED_BYTE;
          fmt.scalable_internal_format = GL_LUMINANCE8;

          pangolin::TypedImage img = pangolin::LoadImage(images[fcid]);
          img_view[1]->SetImage(img);
        } else {
          img_view[1]->Clear();
        }
      }



      pangolin::FinishFrame();
      if (continue_next) {
        if (!measuring_time) {
          // Start measuring time when next_step starts returning true
          start = std::chrono::high_resolution_clock::now();
          measuring_time = true;
        }        
        // stop if there is nothing left to do
        continue_next = next_step();

        if (!continue_next) {
          // Stop measuring time when next_step returns false
          end = std::chrono::high_resolution_clock::now();
          std::chrono::duration<double> elapsed = end - start;
          std::cout << "Total execution time gui: " << elapsed.count() << " seconds" << std::endl;
        }
      } else {
        // if the gui is just idling, make sure we don't burn too much CPU

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
  } else {
    // non-gui mode: Process all frames, then exit

    while (next_step()) {
      // Continue processing frames
    }


  }
  saveTrajectoryButton();
  SVD_APPLY();
  return 0;
}

// Visualize features and related info on top of the image views
void draw_image_overlay(pangolin::View& v, size_t view_id) {
  UNUSED(v);

  auto frame_id =
      static_cast<FrameId>(view_id == 0 ? show_frame1 : show_frame2);
  auto cam_id = static_cast<CamId>(view_id == 0 ? show_cam1 : show_cam2);

  FrameCamId fcid(frame_id, cam_id);

  float text_row = 20;

  if (show_detected) {
    glLineWidth(1.0);
    glColor3f(1.0, 0.0, 0.0);  // red
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (feature_corners.find(fcid) != feature_corners.end()) {
      const KeypointsData& cr = feature_corners.at(fcid);

      for (size_t i = 0; i < cr.corners.size(); i++) {
        Eigen::Vector2d c = cr.corners[i];
        double angle = cr.corner_angles[i];
        pangolin::glDrawCirclePerimeter(c[0], c[1], 3.0);

        Eigen::Vector2d r(3, 0);
        Eigen::Rotation2Dd rot(angle);
        r = rot * r;

        pangolin::glDrawLine(c, c + r);
      }

      pangolin::default_font()
          .Text("Detected %d corners", cr.corners.size())
          .Draw(5, text_row);

    } else {
      glLineWidth(1.0);

      pangolin::default_font().Text("Corners not processed").Draw(5, text_row);
    }
    text_row += 20;
  }

  if (show_matches || show_inliers) {
    glLineWidth(1.0);
    glColor3f(0.0, 0.0, 1.0);  // blue
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    auto o_frame_id =
        static_cast<FrameId>(view_id == 0 ? show_frame2 : show_frame1);
    auto o_cam_id = static_cast<CamId>(view_id == 0 ? show_cam2 : show_cam1);

    FrameCamId o_fcid(o_frame_id, o_cam_id);

    int idx = -1;

    auto it = feature_matches.find(std::make_pair(fcid, o_fcid));

    if (it != feature_matches.end()) {
      idx = 0;
    } else {
      it = feature_matches.find(std::make_pair(o_fcid, fcid));
      if (it != feature_matches.end()) {
        idx = 1;
      }
    }

    if (idx >= 0 && show_matches) {
      if (feature_corners.find(fcid) != feature_corners.end()) {
        const KeypointsData& cr = feature_corners.at(fcid);

        for (size_t i = 0; i < it->second.matches.size(); i++) {
          size_t c_idx = idx == 0 ? it->second.matches[i].first
                                  : it->second.matches[i].second;

          Eigen::Vector2d c = cr.corners[c_idx];
          double angle = cr.corner_angles[c_idx];
          pangolin::glDrawCirclePerimeter(c[0], c[1], 3.0);

          Eigen::Vector2d r(3, 0);
          Eigen::Rotation2Dd rot(angle);
          r = rot * r;

          pangolin::glDrawLine(c, c + r);

          if (show_ids) {
            pangolin::default_font().Text("%d", i).Draw(c[0], c[1]);
          }
        }

        pangolin::default_font()
            .Text("Detected %d matches", it->second.matches.size())
            .Draw(5, text_row);
        text_row += 20;
      }
    }

    glColor3f(0.0, 1.0, 0.0);  // green

    if (idx >= 0 && show_inliers) {
      if (feature_corners.find(fcid) != feature_corners.end()) {
        const KeypointsData& cr = feature_corners.at(fcid);

        for (size_t i = 0; i < it->second.inliers.size(); i++) {
          size_t c_idx = idx == 0 ? it->second.inliers[i].first
                                  : it->second.inliers[i].second;

          Eigen::Vector2d c = cr.corners[c_idx];
          double angle = cr.corner_angles[c_idx];
          pangolin::glDrawCirclePerimeter(c[0], c[1], 3.0);

          Eigen::Vector2d r(3, 0);
          Eigen::Rotation2Dd rot(angle);
          r = rot * r;

          pangolin::glDrawLine(c, c + r);

          if (show_ids) {
            pangolin::default_font().Text("%d", i).Draw(c[0], c[1]);
          }
        }

        pangolin::default_font()
            .Text("Detected %d inliers", it->second.inliers.size())
            .Draw(5, text_row);
        text_row += 20;
      }
    }
  }

  if (show_reprojections) {
    if (image_projections.count(fcid) > 0) {
      glLineWidth(1.0);
      glColor3f(1.0, 0.0, 0.0);  // red
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

      const size_t num_points = image_projections.at(fcid).obs.size();
      double error_sum = 0;
      size_t num_outliers = 0;

      // count up and draw all inlier projections
      for (const auto& lm_proj : image_projections.at(fcid).obs) {
        error_sum += lm_proj->reprojection_error;

        if (lm_proj->outlier_flags != OutlierNone) {
          // outlier point
          glColor3f(1.0, 0.0, 0.0);  // red
          ++num_outliers;
        } else if (lm_proj->reprojection_error >
                   reprojection_error_huber_pixel) {
          // close to outlier point
          glColor3f(1.0, 0.5, 0.0);  // orange
        } else {
          // clear inlier point
          glColor3f(1.0, 1.0, 0.0);  // yellow
        }
        pangolin::glDrawCirclePerimeter(lm_proj->point_reprojected, 3.0);
        pangolin::glDrawLine(lm_proj->point_measured,
                             lm_proj->point_reprojected);
      }

      // only draw outlier projections
      if (show_outlier_observations) {
        glColor3f(1.0, 0.0, 0.0);  // red
        for (const auto& lm_proj : image_projections.at(fcid).outlier_obs) {
          pangolin::glDrawCirclePerimeter(lm_proj->point_reprojected, 3.0);
          pangolin::glDrawLine(lm_proj->point_measured,
                               lm_proj->point_reprojected);
        }
      }

      glColor3f(1.0, 0.0, 0.0);  // red
      pangolin::default_font()
          .Text("Average repr. error (%u points, %u new outliers): %.2f",
                num_points, num_outliers, error_sum / num_points)
          .Draw(5, text_row);
      text_row += 20;
    }
  }

  if (show_epipolar) {
    glLineWidth(1.0);
    glColor3f(0.0, 1.0, 1.0);  // bright teal
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    auto o_frame_id =
        static_cast<FrameId>(view_id == 0 ? show_frame2 : show_frame1);
    auto o_cam_id = static_cast<CamId>(view_id == 0 ? show_cam2 : show_cam1);

    FrameCamId o_fcid(o_frame_id, o_cam_id);

    int idx = -1;

    auto it = feature_matches.find(std::make_pair(fcid, o_fcid));

    if (it != feature_matches.end()) {
      idx = 0;
    } else {
      it = feature_matches.find(std::make_pair(o_fcid, fcid));
      if (it != feature_matches.end()) {
        idx = 1;
      }
    }

    if (idx >= 0 && it->second.inliers.size() > 20) {
      Sophus::SE3d T_this_other =
          idx == 0 ? it->second.T_i_j : it->second.T_i_j.inverse();

      Eigen::Vector3d p0 = T_this_other.translation().normalized();

      int line_id = 0;
      for (double i = -M_PI_2 / 2; i <= M_PI_2 / 2; i += 0.05) {
        Eigen::Vector3d p1(0, sin(i), cos(i));

        if (idx == 0) p1 = it->second.T_i_j * p1;

        p1.normalize();

        std::vector<Eigen::Vector2d, Eigen::aligned_allocator<Eigen::Vector2d>>
            line;
        for (double j = -1; j <= 1; j += 0.001) {
          line.emplace_back(calib_cam.intrinsics[cam_id]->project(
              p0 * j + (1 - std::abs(j)) * p1));
        }

        Eigen::Vector2d c = calib_cam.intrinsics[cam_id]->project(p1);
        pangolin::default_font().Text("%d", line_id).Draw(c[0], c[1]);
        line_id++;

        pangolin::glDrawLineStrip(line);
      }
    }
  }
}

// Update the image views to a given image id
void change_display_to_image(const FrameCamId& fcid) {
  if (0 == fcid.cam_id) {
    // left view
    show_cam1 = 0;
    show_frame1 = fcid.frame_id;
    show_cam1.Meta().gui_changed = true;
    show_frame1.Meta().gui_changed = true;
  } else {
    // right view
    show_cam2 = fcid.cam_id;
    show_frame2 = fcid.frame_id;
    show_cam2.Meta().gui_changed = true;
    show_frame2.Meta().gui_changed = true;
  }
}

// Render the 3D viewer scene of cameras and points
void draw_scene() {
  const FrameCamId fcid1(show_frame1, show_cam1);
  const FrameCamId fcid2(show_frame2, show_cam2);

  const u_int8_t color_camera_current[3]{255, 0, 0};         // red
  const u_int8_t color_camera_left[3]{0, 125, 0};            // dark green
  const u_int8_t color_camera_right[3]{0, 0, 125};           // dark blue
  const u_int8_t color_points[3]{0, 0, 0};                   // black
  const u_int8_t color_old_points[3]{170, 170, 170};         // gray
  const u_int8_t color_selected_left[3]{0, 250, 0};          // green
  const u_int8_t color_selected_right[3]{0, 0, 250};         // blue
  const u_int8_t color_selected_both[3]{0, 250, 250};        // teal
  const u_int8_t color_outlier_observation[3]{250, 0, 250};  // purple

  // render cameras
  if (show_cameras3d) {
    for (const auto& cam : cameras) {
      if (cam.first == fcid1) {
        render_camera(cam.second.T_w_c.matrix(), 3.0f, color_selected_left,
                      0.1f);
      } else if (cam.first == fcid2) {
        render_camera(cam.second.T_w_c.matrix(), 3.0f, color_selected_right,
                      0.1f);
      } else if (cam.first.cam_id == 0) {
        render_camera(cam.second.T_w_c.matrix(), 2.0f, color_camera_left, 0.1f);
      } else {
        render_camera(cam.second.T_w_c.matrix(), 2.0f, color_camera_right,
                      0.1f);
      }
    }
    render_camera(current_pose.matrix(), 2.0f, color_camera_current, 0.1f);
  }

  // render points
  if (show_points3d && landmarks.size() > 0) {
    glPointSize(3.0);
    glBegin(GL_POINTS);
    for (const auto& kv_lm : landmarks) {
      const bool in_cam_1 = kv_lm.second.obs.count(fcid1) > 0;
      const bool in_cam_2 = kv_lm.second.obs.count(fcid2) > 0;

      const bool outlier_in_cam_1 = kv_lm.second.outlier_obs.count(fcid1) > 0;
      const bool outlier_in_cam_2 = kv_lm.second.outlier_obs.count(fcid2) > 0;

      if (in_cam_1 && in_cam_2) {
        glColor3ubv(color_selected_both);
      } else if (in_cam_1) {
        glColor3ubv(color_selected_left);
      } else if (in_cam_2) {
        glColor3ubv(color_selected_right);
      } else if (outlier_in_cam_1 || outlier_in_cam_2) {
        glColor3ubv(color_outlier_observation);
      } else {
        glColor3ubv(color_points);
      }

      pangolin::glVertex(kv_lm.second.p);
    }
    glEnd();
  }

  // render points
  if (show_old_points3d && old_landmarks.size() > 0) {
    glPointSize(3.0);
    glBegin(GL_POINTS);

    for (const auto& kv_lm : old_landmarks) {
      glColor3ubv(color_old_points);
      pangolin::glVertex(kv_lm.second.p);
    }
    glEnd();

  }
  if (show_gt) {
    glColor3ubv(color_camera_current);
    pangolin::glDrawLineStrip(gt_points);
  }
  if (show_vio_pt) {
    glColor3ubv(color_selected_left);
    pangolin::glDrawLineStrip(vio_points);
  }
}

// Load images, calibration, imu, and features / matches if available
void load_data(const std::string& dataset_path, const std::string& calib_path) {
  const std::string timestams_path = dataset_path + "/cam0/data.csv";

  {
    std::ifstream times(timestams_path); // ifstream: read file

    int id = 0;

    while (times) {
      std::string line;
      std::getline(times, line); // line by line read files

      if (line.size() < 20 || line[0] == '#') continue;  // length of line is less than 20 or # or id>2700 skip it 

      {
        std::string timestamp_str = line.substr(0, 19); //from line the 19 front str extraht and then - > timestamp_str -> timestamps
        std::istringstream ss(timestamp_str);
        Timestamp timestamp;
        ss >> timestamp;
        timestamps.push_back(timestamp);
      }

      std::string img_name = line.substr(20, line.size() - 21); //from 21 read the info about images

      for (int i = 0; i < NUM_CAMS; i++) {
        FrameCamId fcid(id, i);

        std::stringstream ss;
        ss << dataset_path << "/cam" << i << "/data/" << img_name;

        images[fcid] = ss.str();  // through framd and camera id to store ' str ' camera data and images info 
      }

      id++;
    }

    std::cerr << "Loaded " << id << " image pairs" << std::endl; // id means frames id
  }

  {
    std::ifstream os(calib_path, std::ios::binary);

    if (os.is_open()) {

      cereal::JSONInputArchive archive(os);
      archive(calib_cam);
      std::cout << "Loaded camera from " << calib_path << " with models ";
      // if do calibration for imu data
      //calib_acc.getParam() = calib_cam.calib_accel_bias;
      //calib_gyro.getParam() = calib_cam.calib_gyro_bias;
      for (const auto& cam : calib_cam.intrinsics) {
        std::cout << cam->name() << " ";
      }
      std::cout << std::endl;
    } else {
      std::cerr << "could not load camera calibration " << calib_path
                << std::endl;
      std::abort();
    }
  }
 // 更新 GUI 显示范围
  show_frame1.Meta().range[1] = images.size() / NUM_CAMS - 1;
  show_frame1.Meta().gui_changed = true;
  show_frame2.Meta().range[1] = images.size() / NUM_CAMS - 1;
  show_frame2.Meta().gui_changed = true;


  DatasetIoInterfacePtr dataset_io =
      DatasetIoFactory::getDatasetIo(dataset_type);
  dataset_io->read(dataset_path);
  // Load the ground true pose data
  
  for (size_t i = 0; i < dataset_io->get_data()->get_gt_state_data().size();
       i++) {
    gt_t_ns.push_back(dataset_io->get_data()->get_gt_timestamps()[i]);
    gt_t_w_i.push_back(dataset_io->get_data()->get_gt_state_data()[i]);  // subset named groundtruth estimation
    gt_points.push_back(dataset_io->get_data()->get_gt_state_data()[i].translation());
  }

  std::cout << "Successfully loaded " << gt_t_w_i.size()<< " ground-true data "
             << std::endl; // True

  load_imu(dataset_io, imu_data_queue, imu_timestamps, calib_acc, calib_gyro);
  std::cout << "Successfully loaded " <<  imu_data_queue.size() << " IMU data "
            << std::endl;
}




///////////////////////////////////////////////////////////////////////////////
/// Here the algorithmically interesting implementation begins
///////////////////////////////////////////////////////////////////////////////

// Execute next step in the overall odometry pipeline. Call this repeatedly
// until it returns false for automatic execution.
bool next_step() {

  if (current_frame >= int(images.size()) / NUM_CAMS) return false;// one frame two cameras

  const Sophus::SE3d T_0_1 = calib_cam.T_i_c[0].inverse() * calib_cam.T_i_c[1];


  if (take_keyframe) {
    take_keyframe = false;

    if (imu) {
      const Vec3 accel_cov = (calib_cam.accel_noise_std).array().square();

      const Vec3 gyro_cov = (calib_cam.gyro_noise_std).array().square();

      if (!initialized) {
          imudata = imu_data_queue.front().second;
          last_state_t_ns = imudata->t_ns; //skip the IMU data before current timestamp
          imu_data_queue.pop();

            // Initialize the pose following the pipeline in basalt
          vel_w_i_init.setZero();
          T_w_i_init.setQuaternion(Eigen::Quaternion<double>::FromTwoVectors(
              imudata->accel, Vec3::UnitZ()));

          first_state.T_w_i = T_w_i_init;
          first_state.vel_w_i = vel_w_i_init;
          frame_states[last_state_t_ns] = first_state;

          imu_measurement.reset(new IntegratedImuMeasurement<double>(
              last_state_t_ns, Eigen::Vector3d::Zero(), Eigen::Vector3d::Zero()));


          while (imudata->t_ns <= last_state_t_ns) {
            imudata = imu_data_queue.front().second;
            imu_data_queue.pop();
          }

          Timestamp curr_timestamp = timestamps[current_frame];
          if(curr_timestamp < imudata->t_ns){
            // do nothing
          }
          else{
            while (imudata->t_ns <= curr_timestamp) {
              imu_measurement->integrate(*imudata, accel_cov, gyro_cov);
              imudata = imu_data_queue.front().second;
              imu_data_queue.pop();
              if (!imudata){std::cout << "Skipping IMU data because of no existing.." << std::endl;break;} 
            }
          
            PoseVelState<double> last_state = frame_states[last_state_t_ns];
            PoseVelState<double> curr_state;
            imu_measurement->predictState(last_state, constants::g, curr_state);

            imu_measurements[curr_timestamp] = *imu_measurement;
            frame_states[curr_timestamp] = curr_state;
            //std::cout << "KeyFrame imu states size: " << frame_states.size() <<std::endl; 
            last_state_t_ns = curr_timestamp;  
          }
          initialized = true;
      }
      else{
          Timestamp curr_timestamp = timestamps[current_frame];

          imu_measurement.reset(new IntegratedImuMeasurement<double>(
              last_state_t_ns, Eigen::Vector3d::Zero(), Eigen::Vector3d::Zero())); // reset the variable(state_delta_) 
          //// if take the calib bias
          //    imu_measurement.reset(new IntegratedImuMeasurement<double>(
          //    last_t_ns, calib_cam.calib_gyro_bias.template head<3>(),
          //    calib_cam.calib_accel_bias.template head<3>()));
          while (imudata->t_ns <= last_state_t_ns) {
            imudata = imu_data_queue.front().second;
            imu_data_queue.pop();
          }

          while (imudata->t_ns <= curr_timestamp) {
            imu_measurement->integrate(*imudata, accel_cov, gyro_cov);
            imudata = imu_data_queue.front().second;
            imu_data_queue.pop();
            if (!imudata){std::cout << "Skipping IMU data because of no existing.." << std::endl;break;} 
          }

          PoseVelState<double> last_state = frame_states[last_state_t_ns];
          PoseVelState<double> curr_state;
          imu_measurement->predictState(last_state, constants::g, curr_state);
          imu_measurements[curr_timestamp] = *imu_measurement;
          frame_states[curr_timestamp] = curr_state;
          //std::cout << "KeyFrame imu states size: " << frame_states.size() <<std::endl; 
          last_state_t_ns = curr_timestamp;
      }
    }
    
    FrameCamId fcidl(current_frame, 0), fcidr(current_frame, 1);

    std::vector<Eigen::Vector2d, Eigen::aligned_allocator<Eigen::Vector2d>>
        projected_points;
    std::vector<TrackId> projected_track_ids;

    project_landmarks(current_pose, calib_cam.intrinsics[0], landmarks,
                      cam_z_threshold, projected_points, projected_track_ids); // project landmarks into the frame of initial camera; but where is landmark from?

    // std::cout << "KF Projected " << projected_track_ids.size() << " points."
    //           << std::endl;

    MatchData md_stereo;
    KeypointsData kdl, kdr;

    pangolin::ManagedImage<uint8_t> imgl = pangolin::LoadImage(images[fcidl]);
    pangolin::ManagedImage<uint8_t> imgr = pangolin::LoadImage(images[fcidr]);

    detectKeypointsAndDescriptors(imgl, kdl, num_features_per_image,
                                  rotate_features);
    detectKeypointsAndDescriptors(imgr, kdr, num_features_per_image,
                                  rotate_features);

    md_stereo.T_i_j = T_0_1;

    Eigen::Matrix3d E;
    computeEssential(T_0_1, E);

    matchDescriptors(kdl.corner_descriptors, kdr.corner_descriptors,
                     md_stereo.matches, feature_match_max_dist,
                     feature_match_test_next_best);

    findInliersEssential(kdl, kdr, calib_cam.intrinsics[0],
                         calib_cam.intrinsics[1], E, 1e-3, md_stereo); // through epipolar constraint to get inliers

    // std::cout << "KF Found " << md_stereo.inliers.size() << " 2d stereo-matches."
    //           << std::endl;

    feature_corners[fcidl] = kdl;
    feature_corners[fcidr] = kdr;
    feature_matches[std::make_pair(fcidl, fcidr)] = md_stereo; // img and img and Match(T_i_j && featureid  featureid)

    LandmarkMatchData md;  // from featureid to trackid;

    find_matches_landmarks(kdl, landmarks, feature_corners, projected_points,
                           projected_track_ids, match_max_dist_2d,
                           feature_match_max_dist, feature_match_test_next_best,
                           md);

    //std::cout << "KF Found " << md.matches.size() << " 3d-2d landmarks matches." << std::endl;

    localize_camera(current_pose, calib_cam.intrinsics[0], kdl, landmarks,
                    reprojection_error_pnp_inlier_threshold_pixel, md);

    current_pose = md.T_w_c;

    cameras[fcidl].T_w_c = current_pose;
    cameras[fcidr].T_w_c = current_pose * T_0_1;


    if (imu) {
          recent_kf_cameras[fcidl].T_w_c = current_pose; // recent cameras : KeyFrame cameras 
          //std::cout<< "add recent KF camera!! "<< std::endl;

          if (recent_kf_cameras.size() > num_latest_frames) {
              FrameId oldest_frame = std::numeric_limits<FrameId>::max();
              FrameCamId remove_fcid;
              for (const auto& kv : recent_kf_cameras) {
                if (kv.first.frame_id < oldest_frame) {
                  oldest_frame = kv.first.frame_id;
                  remove_fcid = kv.first;
                } else {
                  // nop
                }
              }

              // remove the oldest frames
              recent_kf_cameras.erase(remove_fcid);
              removed_fcid_buffer.push_back(remove_fcid);

            }
    }


    add_new_landmarks(fcidl, fcidr, kdl, kdr, calib_cam, md_stereo, md,
                      landmarks, next_landmark_id);

    bool removed_old_keyframes = delete_oldframes(fcidl, max_num_kfs, cameras, landmarks,
                                        old_landmarks, kf_frames,
                                        delete_camera, delete_fid);

    // Ducument the removed keyframe
    if (removed_old_keyframes) {
      Sophus::SE3d T_w_i = delete_camera.T_w_c * calib_cam.T_i_c[0].inverse();
      vio_t_ns.push_back(timestamps[delete_fid]);
      vio_t_w_i.push_back(T_w_i); //imu frame's T relative to world frame and timestamp
      vio_points.push_back(T_w_i.translation());    
    }

    optimize();

    current_pose = cameras[fcidl].T_w_c;

    // update image views
    change_display_to_image(fcidl);
    change_display_to_image(fcidr);

    compute_projections();

    current_frame++;
    return true;

  } else {
    FrameCamId fcidl(current_frame, 0), fcidr(current_frame, 1);

    std::vector<Eigen::Vector2d, Eigen::aligned_allocator<Eigen::Vector2d>>
        projected_points;
    std::vector<TrackId> projected_track_ids;

    project_landmarks(current_pose, calib_cam.intrinsics[0], landmarks,
                      cam_z_threshold, projected_points, projected_track_ids);

    // std::cout << "(no KF)Projected " << projected_track_ids.size() << " points."
    //           << std::endl;


    ///  only take and handle left camera only in (no take key frame) this frame 
    KeypointsData kdl;

    pangolin::ManagedImage<uint8_t> imgl = pangolin::LoadImage(images[fcidl]);

    detectKeypointsAndDescriptors(imgl, kdl, num_features_per_image,
                                  rotate_features);

    feature_corners[fcidl] = kdl;

    LandmarkMatchData md;
    find_matches_landmarks(kdl, landmarks, feature_corners, projected_points,
                           projected_track_ids, match_max_dist_2d,
                           feature_match_max_dist, feature_match_test_next_best,
                           md);

    //std::cout << "Found " << md.matches.size() << " matches." << std::endl;

    localize_camera(current_pose, calib_cam.intrinsics[0], kdl, landmarks,
                    reprojection_error_pnp_inlier_threshold_pixel, md);

    current_pose = md.T_w_c;

    if (int(md.inliers.size()) < new_kf_min_inliers && !opt_running &&
        !opt_finished) {
      take_keyframe = true;
    }

    if (!opt_running && opt_finished) {
      opt_thread->join();  // wait for results from BA in optimization

      landmarks = landmarks_opt;
      removed_fcid_buffer.clear();

      cameras = cameras_opt;

      if(imu){
        // update kf cameras
        for(auto& cam : cameras){
          for(auto& cam_imu : recent_kf_cameras){
            if(cam.first == cam_imu.first){
              //std::cout << " update key_cam !" << std::endl;
              cam_imu.second.T_w_c = cam.second.T_w_c;
            }
          }
        }
      }

      calib_cam = calib_cam_opt;

       if (imu) {
          update_framestates(calib_cam, cameras, timestamps,frame_states, frame_states_opt);
          //update the framestate from optimization
      } else {}
      opt_finished = false;
    }

    // update image views
    change_display_to_image(fcidl);
    change_display_to_image(fcidr);

    current_frame++;
    return true;
  }

}

// Compute reprojections for all landmark observations for visualization and
// outlier removal.
void compute_projections() {
  image_projections.clear();

  for (const auto& kv_lm : landmarks) {
    const TrackId track_id = kv_lm.first;

    for (const auto& kv_obs : kv_lm.second.obs) {
      const FrameCamId& fcid = kv_obs.first;
      const Eigen::Vector2d p_2d_corner =
          feature_corners.at(fcid).corners[kv_obs.second];
      if (cameras.count(fcid)) {
        const Eigen::Vector3d p_c =
            cameras.at(fcid).T_w_c.inverse() * kv_lm.second.p;
        const Eigen::Vector2d p_2d_repoj =
            calib_cam.intrinsics.at(fcid.cam_id)->project(p_c);


        ProjectedLandmarkPtr proj_lm(new ProjectedLandmark);
        proj_lm->track_id = track_id;
        proj_lm->point_measured = p_2d_corner;
        proj_lm->point_reprojected = p_2d_repoj;
        proj_lm->point_3d_c = p_c;
        proj_lm->reprojection_error = (p_2d_corner - p_2d_repoj).norm();

        image_projections[fcid].obs.push_back(proj_lm);
      }
    }

    for (const auto& kv_obs : kv_lm.second.outlier_obs) {
      const FrameCamId& fcid = kv_obs.first;
      const Eigen::Vector2d p_2d_corner =
          feature_corners.at(fcid).corners[kv_obs.second];

      if (cameras.count(fcid)) {
        const Eigen::Vector3d p_c =
            cameras.at(fcid).T_w_c.inverse() * kv_lm.second.p;
        const Eigen::Vector2d p_2d_repoj =
            calib_cam.intrinsics.at(fcid.cam_id)->project(p_c);

        ProjectedLandmarkPtr proj_lm(new ProjectedLandmark);
        proj_lm->track_id = track_id;
        proj_lm->point_measured = p_2d_corner;
        proj_lm->point_reprojected = p_2d_repoj;
        proj_lm->point_3d_c = p_c;
        proj_lm->reprojection_error = (p_2d_corner - p_2d_repoj).norm();

        image_projections[fcid].outlier_obs.push_back(proj_lm);
      }  
    }
  }
}

// Optimize the active map with bundle adjustment
void optimize() {
  size_t num_obs = 0;
  for (const auto& kv : landmarks) {
    num_obs += kv.second.obs.size();
  }

  // std::cerr << "Optimizing map with " << cameras.size() << " cameras, " << "kf_frames size: " << kf_frames.size() << " , " 
  //           << landmarks.size() << " points " << num_obs << " observations, "
  //           << frame_states_opt.size() << " imu state for optimization!"
  //           << std::endl;

  // Fix oldest two cameras to fix SE3 and scale gauge. Making the whole second
  // camera constant is a bit suboptimal, since we only need 1 DoF, but it's
  // simple and the initial poses should be good from calibration.
  FrameId fid = *(kf_frames.begin());
  // std::cout << "fixed fid: " << fid << std::endl;

  // Prepare bundle adjustment
  BundleAdjustmentOptions ba_options;
  ba_options.optimize_intrinsics = ba_optimize_intrinsics;
  ba_options.use_huber = true;
  ba_options.huber_parameter = reprojection_error_huber_pixel;
  ba_options.max_num_iterations = 20;
  ba_options.verbosity_level = ba_verbose;

  calib_cam_opt = calib_cam;
  cameras_opt = cameras;
  landmarks_opt = landmarks;

  if (imu) {
    take_framestates(calib_cam,recent_kf_cameras,timestamps, frame_state,frame_states,frame_states_opt);  //size (frame_states_opt) = size(keyframes cameras)
    // std::cout<< "get frame states optimization !"<<std::endl;
  } else {
    // Do nothing here
  }
   //std::cout<<"optimize !"<<std::endl;
  opt_running = true;

  opt_thread.reset(new std::thread([fid, ba_options] {

  std::set<FrameCamId> fixed_cameras = {{fid, 0}, {fid, 1}};
  if(imu){  // optimize poses landmarks and intrisics
      //std::cout << "start imu projection BA..." << std::endl;
      Imu_Proj_bundle_adjustment(feature_corners, ba_options, fixed_cameras, calib_cam_opt,
                                cameras_opt, landmarks_opt, frame_states_opt, imu_measurements,
                                timestamps);
      // std::cout << "imu projection BA end..." << std::endl;
      }  
  else{                  
      Proj_bundle_adjustment(feature_corners, ba_options, fixed_cameras, calib_cam_opt,
                          cameras_opt, landmarks_opt);}

    opt_finished = true;
    opt_running = false;
  }));

  // Update project info cache
  compute_projections();
}


////Here is from Basalt//
double alignSVD(
    const std::vector<int64_t>& filter_t_ns,
    const std::vector<Eigen::Vector3d,
                      Eigen::aligned_allocator<Eigen::Vector3d>>& filter_t_w_i,
    const std::vector<int64_t>& gt_t_ns,
    std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d>>&
        gt_t_w_i) {
  std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d>>
      est_associations;
  std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d>>
      gt_associations;
  for (auto& fid : kf_frames) {
    FrameCamId temp_fcid(fid, 0);
    vio_points.push_back(
        (cameras.at(temp_fcid).T_w_c * calib_cam.T_i_c[0].inverse())
            .translation());
  }

  for (size_t i = 0; i < filter_t_w_i.size(); i++) {
    int64_t t_ns = filter_t_ns[i];

    size_t j;
    for (j = 0; j < gt_t_ns.size(); j++) {
      if (gt_t_ns.at(j) > t_ns) break;
    }
    j--;

    if (j >= gt_t_ns.size() - 1) {
      continue;
    }

    double dt_ns = t_ns - gt_t_ns.at(j);
    double int_t_ns = gt_t_ns.at(j + 1) - gt_t_ns.at(j);

    BASALT_ASSERT_STREAM(dt_ns >= 0, "dt_ns " << dt_ns);
    BASALT_ASSERT_STREAM(int_t_ns > 0, "int_t_ns " << int_t_ns);

    if (int_t_ns > 1.1e8) continue;

    double ratio = dt_ns / int_t_ns;

    BASALT_ASSERT(ratio >= 0);
    BASALT_ASSERT(ratio < 1);

    Eigen::Vector3d gt = (1 - ratio) * gt_t_w_i[j] + ratio * gt_t_w_i[j + 1];

    gt_associations.emplace_back(gt);
    est_associations.emplace_back(filter_t_w_i[i]);
  }

  int num_kfs = est_associations.size();

  Eigen::Matrix<double, 3, Eigen::Dynamic> gt, est;
  gt.setZero(3, num_kfs);
  est.setZero(3, num_kfs);

  for (size_t i = 0; i < est_associations.size(); i++) {
    gt.col(i) = gt_associations[i];
    est.col(i) = est_associations[i];
  }

  Eigen::Vector3d mean_gt = gt.rowwise().mean();
  Eigen::Vector3d mean_est = est.rowwise().mean();

  gt.colwise() -= mean_gt;
  est.colwise() -= mean_est;

  Eigen::Matrix3d cov = gt * est.transpose();

  Eigen::JacobiSVD<Eigen::Matrix3d> svd(
      cov, Eigen::ComputeFullU | Eigen::ComputeFullV);

  Eigen::Matrix3d S;
  S.setIdentity();

  if (svd.matrixU().determinant() * svd.matrixV().determinant() < 0)
    S(2, 2) = -1;

  Eigen::Matrix3d rot_gt_est = svd.matrixU() * S * svd.matrixV().transpose();
  Eigen::Vector3d trans = mean_gt - rot_gt_est * mean_est;

  Sophus::SE3d T_gt_est(rot_gt_est, trans);
  Sophus::SE3d T_est_gt = T_gt_est.inverse();

  for (size_t i = 0; i < gt_t_w_i.size(); i++) {
    gt_t_w_i[i] = T_est_gt * gt_t_w_i[i];
  }

  double error = 0;
  for (size_t i = 0; i < est_associations.size(); i++) {
    est_associations[i] = T_gt_est * est_associations[i];
    Eigen::Vector3d res = est_associations[i] - gt_associations[i];

    error += res.transpose() * res;
  }

  error /= est_associations.size();
  error = std::sqrt(error);
  return error;
}
////Here is from Basalt//

double SVD_APPLY() {
  for (auto& fid : kf_frames) {
    FrameCamId temp_fcid(fid, 0);
    vio_points.push_back(
        (cameras.at(temp_fcid).T_w_c * calib_cam.T_i_c[0].inverse())
            .translation());
  }
  double error = alignSVD(vio_t_ns, vio_points, gt_t_ns, gt_points);
  return error;
}

void saveTrajectoryButton() {
  
  for (auto& fid : kf_frames) {
    FrameCamId temp_fcid(fid, 0);
    vio_t_ns.push_back(timestamps[fid]);
    vio_t_w_i.push_back(
        (cameras.at(temp_fcid).T_w_c * calib_cam.T_i_c[0].inverse()));
  }
    if(imu){
          std::ofstream os("tum_benchmark_tools/vio_trajectory.txt");

          for (size_t i = 0; i < vio_t_ns.size(); i++) {
              const Sophus::SE3d& pose = vio_t_w_i[i];
              os << std::scientific << std::setprecision(18) << vio_t_ns[i] << " "
                << pose.translation().x() << " " << pose.translation().y() << " "
                << pose.translation().z() << " " << pose.unit_quaternion().x() << " "
                << pose.unit_quaternion().y() << " "<< pose.unit_quaternion().z() << " "
                << pose.unit_quaternion().w() << std::endl;
          }
    }
    else{
          std::ofstream os("tum_benchmark_tools/vo_trajectory.txt");

          for (size_t i = 0; i < vio_t_ns.size(); i++) {
              const Sophus::SE3d& pose = vio_t_w_i[i];
              os << std::scientific << std::setprecision(18) << vio_t_ns[i] << " "
                << pose.translation().x() << " " << pose.translation().y() << " "
                << pose.translation().z() << " " << pose.unit_quaternion().x() << " "
                << pose.unit_quaternion().y() << " "<< pose.unit_quaternion().z() << " "
                << pose.unit_quaternion().w() << std::endl;
          }
    }
          std::ofstream os("tum_benchmark_tools/gt_trajectory.txt");

          for (size_t i = 0; i < gt_t_ns.size(); i++) {
              const Sophus::SE3d& pose = gt_t_w_i[i];
              os << std::scientific << std::setprecision(18) << gt_t_ns[i] << " "
                << pose.translation().x() << " " << pose.translation().y() << " "
                << pose.translation().z() << " " << pose.unit_quaternion().x() << " "
                << pose.unit_quaternion().y() << " "<< pose.unit_quaternion().z() << " "
                << pose.unit_quaternion().w() << std::endl;
          }


    std::cout << "Saved trajectory in Euroc Dataset format in trajectory.txt"
              << std::endl;

}