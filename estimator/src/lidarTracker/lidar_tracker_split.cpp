/*******************************************************
 * Copyright (C) 2019, Aerial Robotics Group, Hong Kong University of Science and Technology
 *
 * This file is part of VINS.
 *
 * Licensed under the GNU General Public License v3.0;
 * you may not use this file except in compliance with the License.
 *
 * Author: Qin Tong (qintonguav@gmail.com)
 *******************************************************/

#include "lidar_tracker_split.h"
#include "../factor/lidar_factor.hpp"

using namespace common;

LidarTracker::LidarTracker()
{
    ROS_INFO("Tracker begin");
}

// TODO: may use closed-form ICP to do
Pose LidarTracker::trackCloud(const cloudFeature &prev_cloud_feature,
                              const cloudFeature &cur_cloud_feature,
                              const Pose &pose_ini)
{
    // step 1: prev feature and kdtree
    PointICloudPtr corner_points_last = boost::make_shared<PointICloud>(prev_cloud_feature.find("corner_points_less_sharp")->second);
    PointICloudPtr surf_points_last = boost::make_shared<PointICloud>(prev_cloud_feature.find("surf_points_less_flat")->second);
    pcl::KdTreeFLANN<PointI>::Ptr kdtree_corner_last(new pcl::KdTreeFLANN<PointI>());
    pcl::KdTreeFLANN<PointI>::Ptr kdtree_surf_last(new pcl::KdTreeFLANN<PointI>());
    kdtree_corner_last->setInputCloud(corner_points_last);
    kdtree_surf_last->setInputCloud(surf_points_last);

    // step 2: current feature
    PointICloudPtr corner_points_sharp = boost::make_shared<PointICloud>(cur_cloud_feature.find("corner_points_sharp")->second);
    PointICloudPtr surf_points_flat = boost::make_shared<PointICloud>(cur_cloud_feature.find("surf_points_flat")->second);

    // step 3: set initial pose
    double para_pose[SIZE_POSE] = {pose_ini.t_(0), pose_ini.t_(1), pose_ini.t_(2),
                                   pose_ini.q_.x(), pose_ini.q_.y(), pose_ini.q_.z(), pose_ini.q_.w()};
    std::vector<double *> para_ids;
    para_ids.push_back(para_pose);

    TicToc t_solver;

    // extract surf features to optimize (z, roll, pitch)
    for (size_t iter_cnt = 0; iter_cnt < 2; iter_cnt++)
    {
        ceres::Problem problem;
        ceres::Solver::Summary summary;
        ceres::LossFunction *loss_function = new ceres::HuberLoss(0.1);

        ceres::Solver::Options options;
        options.linear_solver_type = ceres::DENSE_SCHUR;
        options.max_num_iterations = 6;
        options.minimizer_progress_to_stdout = false;
        options.check_gradients = false;
        options.gradient_check_relative_precision = 1e-4;

        PoseLocalParameterization *local_parameterization = new PoseLocalParameterization();
        local_parameterization->setParameter();
        local_parameterization->V_update_(0, 0) = 0; // x
        local_parameterization->V_update_(1, 1) = 0; // y
        local_parameterization->V_update_(5, 5) = 0; // yaw
        problem.AddParameterBlock(para_pose, SIZE_POSE, local_parameterization);

        std::vector<PointPlaneFeature> surf_scan_features;
        Pose pose_local;
        pose_local.t_ = Eigen::Vector3d(para_pose[0], para_pose[1], para_pose[2]);
        pose_local.q_ = Eigen::Quaterniond(para_pose[6], para_pose[3], para_pose[4], para_pose[5]);
        f_extract_.matchSurfFromScan(kdtree_surf_last, *surf_points_last, *surf_points_flat, pose_local, surf_scan_features);
        if (surf_scan_features.size() < 10)
        {
            printf("less correspondence! *************************************************\n");
            break;
        }
        for (auto &feature : surf_scan_features)
        {
            const size_t &idx = feature.idx_;
            const Eigen::Vector3d &p_data = feature.point_;
            const Eigen::Vector4d &coeff = feature.coeffs_;
            double s;
            if (DISTORTION)
                s = (surf_points_flat->points[idx].intensity - int(surf_points_flat->points[idx].intensity)) / SCAN_PERIOD;
            else
                s = 1.0;
            LidarScanPlaneNormFactor *f = new LidarScanPlaneNormFactor(p_data, coeff, s);
            problem.AddResidualBlock(f, loss_function, para_pose);
        }
        ceres::Solve(options, &problem, &summary);
    }

    // extract corner features to optimize (x, y, yaw)
    for (size_t iter_cnt = 0; iter_cnt < 2; iter_cnt++)
    {
        ceres::Problem problem;
        ceres::Solver::Summary summary;
        ceres::LossFunction *loss_function = new ceres::HuberLoss(0.1);

        ceres::Solver::Options options;
        options.linear_solver_type = ceres::DENSE_SCHUR;
        options.max_num_iterations = 6;
        options.minimizer_progress_to_stdout = false;
        options.check_gradients = false;
        options.gradient_check_relative_precision = 1e-4;

        PoseLocalParameterization *local_parameterization = new PoseLocalParameterization();
        local_parameterization->setParameter();
        local_parameterization->V_update_(2, 2) = 0; // z
        local_parameterization->V_update_(3, 3) = 0; // roll
        local_parameterization->V_update_(4, 4) = 0; // pitch
        problem.AddParameterBlock(para_pose, SIZE_POSE, local_parameterization);

        TicToc t_prepare;
        std::vector<PointPlaneFeature> corner_scan_features;
        Pose pose_local;
        pose_local.t_ = Eigen::Vector3d(para_pose[0], para_pose[1], para_pose[2]);
        pose_local.q_ = Eigen::Quaterniond(para_pose[6], para_pose[3], para_pose[4], para_pose[5]);                  
        f_extract_.matchCornerFromScan(kdtree_corner_last, *corner_points_last, *corner_points_sharp, pose_local, corner_scan_features);
        if (corner_scan_features.size() < 10)
        {
            printf("less correspondence! *************************************************\n");
            break;
        }

        for (auto &feature : corner_scan_features)
        {
            const size_t &idx = feature.idx_;
            const Eigen::Vector3d &p_data = feature.point_;
            const Eigen::Vector4d &coeff = feature.coeffs_;
            double s;
            if (DISTORTION)
                s = (corner_points_sharp->points[idx].intensity - int(corner_points_sharp->points[idx].intensity)) / SCAN_PERIOD;
            else
                s = 1.0;
            LidarScanPlaneNormFactor *f = new LidarScanPlaneNormFactor(p_data, coeff, s);
            problem.AddResidualBlock(f, loss_function, para_pose);
        }
        TicToc t_solver;
        ceres::Solve(options, &problem, &summary);      
    }

    printf("ceres_split solver: %f\n", t_solver.toc());
    Pose pose_prev_cur(Eigen::Quaterniond(para_pose[6], para_pose[3], para_pose[4], para_pose[5]), 
                       Eigen::Vector3d(para_pose[0], para_pose[1], para_pose[2]));
    return pose_prev_cur;
}

