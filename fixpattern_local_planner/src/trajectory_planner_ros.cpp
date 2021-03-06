/* Copyright(C) Gaussian Robot. All rights reserved.
*/

/**
 * @file trajectory_planner_ros.cpp
 * @brief fixpattern local planner
 * @author cameron<chenkan@gs-robot.com>
 * @version 1.0.0.0
 * @date 2015-08-04
 */

#include "fixpattern_local_planner/trajectory_planner_ros.h"

#include <sys/time.h>
#include <boost/tokenizer.hpp>
#include <Eigen/Core>
#include <ros/console.h>
#include <fixpattern_local_planner/goal_functions.h>
#include <nav_msgs/Path.h>
//#include <pcl_conversions/pcl_conversions.h>

#include <cmath>
#include <string>
#include <algorithm>
#include <vector>

namespace fixpattern_local_planner {


FixPatternTrajectoryPlannerROS::FixPatternTrajectoryPlannerROS()
  : world_model_(NULL), tc_(NULL), la_(NULL), costmap_ros_(NULL), tf_(NULL), initialized_(false), odom_helper_("odom") {
  rotate_to_goal_k_ = 0.9;
  last_rotate_to_goal_dir_ = 0;
  last_target_yaw_ = 0.0;
  try_rotate_ = 0;
  GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] FixPatternTrajectoryPlannerROS object created");
}

FixPatternTrajectoryPlannerROS::FixPatternTrajectoryPlannerROS(std::string name, tf::TransformListener* tf, costmap_2d::Costmap2DROS* costmap_ros)
  : world_model_(NULL), tc_(NULL), la_(NULL), costmap_ros_(NULL), tf_(NULL), initialized_(false), odom_helper_("odom") {
  // initialize the planner
  initialize(name, tf, costmap_ros);
}

void FixPatternTrajectoryPlannerROS::initialize(std::string name, tf::TransformListener* tf, costmap_2d::Costmap2DROS* costmap_ros) {
  if (!initialized_) {
    GAUSSIAN_INFO("[Local Planner] FixPatternTrajectoryPlannerROS initialize");

    ros::NodeHandle private_nh("~/" + name);
    g_plan_pub_ = private_nh.advertise<nav_msgs::Path>("global_plan", 1);
    l_plan_pub_ = private_nh.advertise<nav_msgs::Path>("local_plan", 1);
//    traj_cloud_pub_.advertise(private_nh, "trajectory_cloud", 1);

    tf_ = tf;
    costmap_ros_ = costmap_ros;
    rot_stopped_velocity_ = 1e-2;
    trans_stopped_velocity_ = 1e-2;
    int num_calc_footprint_cost;
    double sim_time, sim_granularity, front_safe_sim_time, front_safe_sim_granularity;
    int vtheta_samples;
    double pdist_scale, gdist_scale, occdist_scale;
    double max_vel_x, min_vel_x;
    double backup_vel;
    std::string world_model_type;
    rotating_to_goal_ = false;
    rotating_to_goal_done_ = false;

    // initialize the copy of the costmap the controller will use
    costmap_ = costmap_ros_->getCostmap();

    global_frame_ = costmap_ros_->getGlobalFrameID();
    robot_base_frame_ = costmap_ros_->getBaseFrameID();
    private_nh.param("prune_plan", prune_plan_, true); //

    private_nh.param("p14", latch_xy_goal_tolerance_, false);
    private_nh.param("p13", p_yaw_goal_tolerance_, 0.05);
    GAUSSIAN_INFO("[LOCAL PLANNER] yaw_goal_tolerance: %lf", p_yaw_goal_tolerance_);
    private_nh.param("p12", p_xy_goal_tolerance_, 0.50);
    GAUSSIAN_INFO("[LOCAL PLANNER] xy_goal_tolerance: %lf", p_xy_goal_tolerance_);
    private_nh.param("p10", acc_lim_x_, 2.5);
    private_nh.param("p11", acc_lim_y_, 2.5);
    private_nh.param("p9", acc_lim_theta_, acc_lim_theta_);
    resetGoalTolerance();
    // Since I screwed up nicely in my documentation, I'm going to add errors
    // informing the user if they've set one of the wrong parameters
    if (private_nh.hasParam("acc_limit_x")) {
      GAUSSIAN_ERROR("You are using acc_limit_x where you should be using acc_lim_x."
                " Please change your configuration files appropriately. The documentation used to be wrong on this, sorry for any confusion.");
     }

    if (private_nh.hasParam("acc_limit_y")) {
      GAUSSIAN_ERROR("You are using acc_limit_y where you should be using acc_lim_y."
                " Please change your configuration files appropriately. The documentation used to be wrong on this, sorry for any confusion.");
     }

    if (private_nh.hasParam("acc_limit_th")) {
      GAUSSIAN_ERROR("You are using acc_limit_th where you should be using acc_lim_th."
                " Please change your configuration files appropriately. The documentation used to be wrong on this, sorry for any confusion.");
     }

    // Assuming this planner is being run within the navigation stack, we can
    // just do an upward search for the frequency at which its being run. This
    // also allows the frequency to be overwritten locally.
    std::string controller_frequency_param_name;
    if (!private_nh.searchParam("controller_frequency", controller_frequency_param_name)) {
      sim_period_ = 0.05;
    } else {
      double controller_frequency = 0;
      private_nh.param(controller_frequency_param_name, controller_frequency, 20.0);
      if (controller_frequency > 0) {
        sim_period_ = 1.0 / controller_frequency;
      } else {
        GAUSSIAN_WARN("A controller_frequency less than 0 has been set. Ignoring the parameter, assuming a rate of 20Hz");
        sim_period_ = 0.05;
      }
    }
    GAUSSIAN_INFO("Sim period is set to %.2f", sim_period_);

    private_nh.param("p15", num_calc_footprint_cost, 5);
    private_nh.param("p16", rotate_to_goal_k_, 1.2);
    private_nh.param("p17", max_rotate_try_times_, 1);
    private_nh.param("p18", sim_time, 6.0);
    private_nh.param("p19", sim_granularity, 0.025);
    private_nh.param("p20", front_safe_sim_time, 1.0);
    private_nh.param("p21", front_safe_sim_granularity, 1.0);
    private_nh.param("p25", vtheta_samples, 20);

    private_nh.param("p22", pdist_scale, 0.6);
    private_nh.param("p23", gdist_scale, 0.8);
    private_nh.param("p24", occdist_scale, 0.01);
    private_nh.param("p26", final_vel_ratio_, 1.0);
    private_nh.param("p27", final_goal_dis_th_, 1.0);

    private_nh.param("p1", max_vel_x, 0.5);
    private_nh.param("p2", min_vel_x, 0.08);
    private_nh.param("p4", max_vel_theta_, 0.6);
    private_nh.param("p3", min_vel_theta_, -0.6);
    private_nh.param("p8", min_in_place_rotational_vel_, 0.1);
//    private_nh.param("min_in_place_rotational_vel", min_in_place_vel_th_, 0.4);
    private_nh.param("p6", min_vel_abs_th_, 0.1);
    private_nh.param("p7", min_hightlight_dis_, 0.5);
    
    reached_goal_ = false;
    backup_vel = -0.1;
    if (private_nh.getParam("backup_vel", backup_vel)) {
      GAUSSIAN_WARN("The backup_vel parameter has been deprecated in favor of the escape_vel parameter. To switch, just change the parameter name in your configuration files.");
    }
    // if both backup_vel and escape_vel are set... we'll use escape_vel
    private_nh.getParam("escape_vel", backup_vel);

    if (backup_vel >= 0.0) {
      GAUSSIAN_WARN("You've specified a positive escape velocity."
               " This is probably not what you want and will cause the robot to move forward instead of backward."
               " You should probably change your escape_vel parameter to be negative");
    }

    private_nh.param("world_model", world_model_type, std::string("costmap"));

    ROS_ASSERT_MSG(world_model_type == "costmap", "At this time, only costmap world models are supported by this controller");
    world_model_ = new CostmapModel(*costmap_);

    footprint_spec_ = costmap_ros_->getRobotFootprint();

    tc_ = new TrajectoryPlanner(*world_model_, *costmap_, footprint_spec_,
                                acc_lim_x_, acc_lim_y_, acc_lim_theta_, num_calc_footprint_cost,
                                sim_time, sim_granularity, front_safe_sim_time, front_safe_sim_granularity,
                                vtheta_samples,
                                pdist_scale, gdist_scale, occdist_scale, 
                                max_vel_x, min_vel_x, max_vel_theta_, min_vel_theta_, min_in_place_rotational_vel_,
                                backup_vel, min_hightlight_dis_, final_vel_ratio_, final_goal_dis_th_);

    la_ = new LookAheadPlanner(*world_model_, *costmap_, footprint_spec_,
                               sim_granularity, acc_lim_x_, acc_lim_y_, acc_lim_theta_,
                               max_vel_x, min_vel_x, max_vel_theta_, min_vel_theta_, min_in_place_rotational_vel_);

    initialized_ = true;

  } else {
    GAUSSIAN_WARN("This planner has already been initialized, doing nothing");
  }
}

FixPatternTrajectoryPlannerROS::~FixPatternTrajectoryPlannerROS() {
  // make sure to clean things up
//  delete dsrv_;

  if (tc_ != NULL)
    delete tc_;

  if (la_ != NULL)
    delete la_;

  if (world_model_ != NULL)
    delete world_model_;
}

bool FixPatternTrajectoryPlannerROS::stopWithAccLimits(PlannerType planner_type, const tf::Stamped<tf::Pose>& global_pose, const tf::Stamped<tf::Pose>& robot_vel, geometry_msgs::Twist* cmd_vel) {
  // slow down with the maximum possible acceleration... we should really use the frequency that we're running at to determine what is feasible
  // but we'll use a tenth of a second to be consistent with the implementation of the local planner.
  double vx = sign(robot_vel.getOrigin().x()) * std::max(0.0, (fabs(robot_vel.getOrigin().x()) - acc_lim_x_ * sim_period_));
  double vy = sign(robot_vel.getOrigin().y()) * std::max(0.0, (fabs(robot_vel.getOrigin().y()) - acc_lim_y_ * sim_period_));

  double vel_yaw = tf::getYaw(robot_vel.getRotation());
  double vth = sign(vel_yaw) * std::max(0.0, (fabs(vel_yaw) - acc_lim_theta_ * sim_period_));

  // we do want to check whether or not the command is valid
  double yaw = tf::getYaw(global_pose.getRotation());
  bool valid_cmd = true;
  if (planner_type == TRAJECTORY_PLANNER) {
    valid_cmd = tc_->CheckTrajectoryWithSimTime(global_pose.getOrigin().getX(), global_pose.getOrigin().getY(), yaw,
                                                robot_vel.getOrigin().getX(), robot_vel.getOrigin().getY(), vel_yaw, vx, vy, vth, 2.0);
  } else if (planner_type == LOOKAHEAD_PLANNER) {
    valid_cmd = la_->CheckTrajectory(global_pose.getOrigin().getX(), global_pose.getOrigin().getY(), yaw,
                                     robot_vel.getOrigin().getX(), robot_vel.getOrigin().getY(), vel_yaw, vx, vy, vth);
  }

  // if we have a valid command, we'll pass it on, otherwise we'll command all zeros
  if (valid_cmd) {
    ROS_DEBUG("Slowing down... using vx, vy, vth: %.2f, %.2f, %.2f", vx, vy, vth);
    GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] stopWithAccLimits: vx = %lf, vth = %lf", vx, vth);
    cmd_vel->linear.x = vx;
    cmd_vel->linear.y = vy;
//    cmd_vel->angular.z = vth;
    cmd_vel->angular.z = 0.0;
    return true;
  }

  cmd_vel->linear.x = 0.0;
  cmd_vel->linear.y = 0.0;
  cmd_vel->angular.z = 0.0;
  return false;
}

bool FixPatternTrajectoryPlannerROS::rotateToGoal(PlannerType planner_type, const tf::Stamped<tf::Pose>& global_pose, const tf::Stamped<tf::Pose>& robot_vel, double goal_th, geometry_msgs::Twist* cmd_vel, int rotate_direction) {
  double yaw = tf::getYaw(global_pose.getRotation());
  double vel_yaw = tf::getYaw(robot_vel.getRotation());
  cmd_vel->linear.x = 0;
  cmd_vel->linear.y = 0;
  double ang_diff = angles::shortest_angular_distance(yaw, goal_th);
  // if rotate_direction == 0, we will just jump over
  // if different from last_rotate_to_goal_dir_, we'll follow last_rotate_to_goal_dir_
  if (rotate_direction != 0 &&
      (last_rotate_to_goal_dir_ == 0 || last_rotate_to_goal_dir_ * rotate_direction > 0)
      && ang_diff * rotate_direction < 0) {
    ang_diff += -1 * sign(ang_diff) * 2 * M_PI;
  }
  if (last_rotate_to_goal_dir_ != 0 && ang_diff * last_rotate_to_goal_dir_ < 0.0) {
    ang_diff = sign(ang_diff) * (fabs(ang_diff) - 2 * M_PI);
  }
  last_rotate_to_goal_dir_ = ang_diff < 0.0 ? -1 : 1;;

  double v_theta_samp = ang_diff > 0.0 ?
      std::min(max_vel_theta_, std::max(min_in_place_rotational_vel_, ang_diff * rotate_to_goal_k_)) :
      std::max(min_vel_theta_, std::min(-1.0 * min_in_place_rotational_vel_, ang_diff * rotate_to_goal_k_));

  // take the acceleration limits of the robot into account
  double max_acc_vel = fabs(vel_yaw) + acc_lim_theta_ * sim_period_;
  double min_acc_vel = fabs(vel_yaw) - acc_lim_theta_ * sim_period_;

  v_theta_samp = sign(v_theta_samp) * std::min(std::max(fabs(v_theta_samp), min_acc_vel), max_acc_vel);

  // we also want to make sure to send a velocity that allows us to stop when we reach the goal given our acceleration limits
  double max_speed_to_stop = sqrt(2 * acc_lim_theta_ * fabs(ang_diff));

  v_theta_samp = sign(v_theta_samp) * std::min(max_speed_to_stop, fabs(v_theta_samp));

  // Re-enforce min_in_place_rotational_vel_.  It is more important than the acceleration limits.
  v_theta_samp = v_theta_samp > 0.0
      ? std::min(max_vel_theta_, std::max(min_in_place_rotational_vel_, v_theta_samp))
      : std::max(min_vel_theta_, std::min(-1.0 * min_in_place_rotational_vel_, v_theta_samp));

  double angle_diff = angles::shortest_angular_distance(yaw, goal_th);
  GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] rotate to goal: angle_diff = %lf", angle_diff);
  if(fabs(angle_diff) < 0.15) {
    v_theta_samp *= 0.30;
  } else if(fabs(angle_diff) < 0.35) {
    v_theta_samp *= 0.45;
  }
  if (fabs(v_theta_samp) < min_vel_abs_th_) {
    v_theta_samp = v_theta_samp < 0.0 ? -1.0 * min_vel_abs_th_ : min_vel_abs_th_;
  }
  // we still want to lay down the footprint of the robot and check if the action is legal
  bool valid_cmd = true;
  if (planner_type == TRAJECTORY_PLANNER) {
    valid_cmd = tc_->checkTrajectory(global_pose.getOrigin().getX(),
                                     global_pose.getOrigin().getY(), yaw,
                                     robot_vel.getOrigin().getX(),
                                     robot_vel.getOrigin().getY(),
                                     vel_yaw, 0.0, 0.0, v_theta_samp);
  } else if (planner_type == LOOKAHEAD_PLANNER) {
    valid_cmd = la_->CheckTrajectory(global_pose.getOrigin().getX(),
                                     global_pose.getOrigin().getY(), yaw,
                                     robot_vel.getOrigin().getX(),
                                     robot_vel.getOrigin().getY(),
                                     vel_yaw, 0.0, 0.0, v_theta_samp);
  }

  ROS_DEBUG("Moving to desired goal orientation, th cmd: %.2f, valid_cmd: %d", v_theta_samp, valid_cmd);

  if (valid_cmd) {
    try_rotate_ = 0;
    cmd_vel->angular.z = v_theta_samp;
    return true;
  } else {
    if (try_rotate_ >= max_rotate_try_times_) {
      last_rotate_to_goal_dir_ *= -1;
    }
    try_rotate_ += 1;
  }

  cmd_vel->angular.z = 0.0;
  return false;
}

bool FixPatternTrajectoryPlannerROS::needBackward(PlannerType planner_type, const tf::Stamped<tf::Pose>& global_pose, const tf::Stamped<tf::Pose>& robot_vel, geometry_msgs::Twist* cmd_vel) {
  double yaw = tf::getYaw(global_pose.getRotation());
  double vel_yaw = tf::getYaw(robot_vel.getRotation());
  cmd_vel->linear.x = 0.0;
  cmd_vel->linear.y = 0.0;
  cmd_vel->angular.z = 0.0;
  // we want to lay down the footprint of the robot and check if the action is legal
  bool valid_cmd = false;
  // rotating left
  double v_theta_samp = min_in_place_rotational_vel_;
  if (planner_type == TRAJECTORY_PLANNER) {
    valid_cmd |= tc_->checkTrajectory(global_pose.getOrigin().getX(),
                                      global_pose.getOrigin().getY(), yaw,
                                      robot_vel.getOrigin().getX(),
                                      robot_vel.getOrigin().getY(),
                                      vel_yaw, 0.0, 0.0, v_theta_samp);
  } else if (planner_type == LOOKAHEAD_PLANNER) {
    valid_cmd |= la_->CheckTrajectory(global_pose.getOrigin().getX(),
                                      global_pose.getOrigin().getY(), yaw,
                                      robot_vel.getOrigin().getX(),
                                      robot_vel.getOrigin().getY(),
                                      vel_yaw, 0.0, 0.0, v_theta_samp);
  }
  // rotating right
  v_theta_samp = -min_in_place_rotational_vel_;
  if (planner_type == TRAJECTORY_PLANNER) {
    valid_cmd |= tc_->checkTrajectory(global_pose.getOrigin().getX(),
                                      global_pose.getOrigin().getY(), yaw,
                                      robot_vel.getOrigin().getX(),
                                      robot_vel.getOrigin().getY(),
                                      vel_yaw, 0.0, 0.0, v_theta_samp);
  } else if (planner_type == LOOKAHEAD_PLANNER) {
    valid_cmd |= la_->CheckTrajectory(global_pose.getOrigin().getX(),
                                      global_pose.getOrigin().getY(), yaw,
                                      robot_vel.getOrigin().getX(),
                                      robot_vel.getOrigin().getY(),
                                      vel_yaw, 0.0, 0.0, v_theta_samp);
  }

  if (valid_cmd) {
    return false;
  } else {
    cmd_vel->linear.x = -0.1;
    cmd_vel->linear.y = 0.0;
    cmd_vel->angular.z = 0.0;
    GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] need backward!");
    return true;
  }
}

double getPoseDistance(geometry_msgs::PoseStamped start_pose, geometry_msgs::PoseStamped end_pose) {
    return hypot(start_pose.pose.position.x- end_pose.pose.position.x, start_pose.pose.position.y- end_pose.pose.position.y);
}

double getPlanLength(const std::vector<fixpattern_path::PathPoint>& plan) {
  if (plan.size() <= 0)
    return 0.0;
  double length = 0.0;
  for (int i = 0; i < plan.size() - 1; ++i)
    length += plan.at(i).DistanceToPoint(plan.at(i + 1));
  return length;
}

bool FixPatternTrajectoryPlannerROS::setPlan(const std::vector<fixpattern_path::PathPoint>& orig_global_plan, const std::string& orig_frame_id) {
  if (!isInitialized()) {
    GAUSSIAN_ERROR("This planner has not been initialized, please call initialize() before using this planner");
    return false;
  }

  global_goal_ = fixpattern_path::PathPointToGeometryPoseStamped(orig_global_plan.back());
  global_goal_.header.frame_id = orig_frame_id;

  std::vector<fixpattern_path::PathPoint> new_global_plan = orig_global_plan;
  // if global plan is too short, we will extend it to avoid robot shaking when ariving global goal
  if (getPlanLength(new_global_plan) < final_goal_dis_th_ && new_global_plan.size() > 2) {
   // extend path
    double yaw = fixpattern_path::CalculateDirection(new_global_plan.front(), new_global_plan.back());
    for (int i = 0; i < 10; ++i) {
      new_global_plan.push_back(new_global_plan.back());
      new_global_plan.back().position.x += 0.05 * cos(yaw);
      new_global_plan.back().position.y += 0.05 * sin(yaw);
    }
    final_goal_extended_ = true;
  } else {
    final_goal_extended_ = false;
  }

//  GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] orig plan size = %zu; new plan size = %zu", orig_global_plan.size(), new_global_plan.size());

  // reset the global plan
  global_plan_.clear();
  for (const auto& p : new_global_plan) {
    geometry_msgs::PoseStamped pose = fixpattern_path::PathPointToGeometryPoseStamped(p);
    pose.header.frame_id = orig_frame_id;
    global_plan_.push_back(pose);
  }
  fixpattern_path_ = orig_global_plan;

  // // when we get a new plan, we also want to clear any latch we may have on goal tolerances
  // xy_tolerance_latch_ = false;
  // // reset the at goal flag
  // reached_goal_ = false;
  return true;
}

bool FixPatternTrajectoryPlannerROS::computeVelocityCommands(PlannerType planner_type, geometry_msgs::Twist* cmd_vel) {
  if (!initialized_) {
    GAUSSIAN_ERROR("This planner has not been initialized, please call initialize() before using this planner");
    return false;
  }

  if (fixpattern_path_.size() == 0) {
    GAUSSIAN_ERROR("[FIXPATTERN LOCAL PLANNER] fixpattern_path_.size() == 0");
    return false;
  }

  std::vector<geometry_msgs::PoseStamped> local_plan;
  tf::Stamped<tf::Pose> global_pose;
  if (!costmap_ros_->getRobotPose(global_pose)) {
    GAUSSIAN_ERROR("[FIXPATTERN LOCAL PLANNER] costmap_ros_->getRobotPose failed");
    return false;
  }

  std::vector<geometry_msgs::PoseStamped> transformed_plan;
  // get the global plan in our frame
  if (!transformGlobalPlan(*tf_, global_plan_, global_pose, *costmap_,
                           global_frame_, transformed_plan, fixpattern_path_.front().highlight)) {
    GAUSSIAN_ERROR("Could not transform the global plan to the frame of the controller");
    return false;
  }

  // now we'll prune the plan based on the position of the robot
  if (prune_plan_)
    prunePlan(global_pose, transformed_plan, global_plan_);

  tf::Stamped<tf::Pose> drive_cmds;
  drive_cmds.frame_id_ = robot_base_frame_;

  tf::Stamped<tf::Pose> robot_vel;
  odom_helper_.getRobotVel(robot_vel);

  /* For timing uncomment
     struct timeval start, end;
     double start_t, end_t, t_diff;
     gettimeofday(&start, NULL);
     */

  // if the global plan passed in is empty... we won't do anything
  if (transformed_plan.empty()) {
    GAUSSIAN_ERROR("[FIXPATTERN LOCAL PLANNER] transformed_plan is empty");
    return false;
  }

  // we assume the global goal is the last point in the global plan
  geometry_msgs::PoseStamped goal_pose = transformed_plan.back();
  if (final_goal_extended_) {
    goal_pose = global_goal_;
  }

  // transform frame for global_goal, into local frame
  // local fram is 'global_frame_', cause this is in local planner
  tf::Stamped<tf::Pose> tf_global_goal;
  tf::Stamped<tf::Pose> tf_front_point;

  poseStampedMsgToTF(global_goal_, tf_global_goal);
  geometry_msgs::PoseStamped front_point = global_plan_.front();
  poseStampedMsgToTF(front_point, tf_front_point);
  tf::StampedTransform plan_to_global_transform;
  tf_->lookupTransform(global_frame_, ros::Time(),
                       global_goal_.header.frame_id, global_goal_.header.stamp,
                       global_goal_.header.frame_id, plan_to_global_transform);
  tf::Stamped<tf::Pose> goal_point;
  goal_point.setData(plan_to_global_transform * tf_global_goal);
  goal_point.stamp_ = plan_to_global_transform.stamp_;
  goal_point.frame_id_ = global_frame_;
  tf::Stamped<tf::Pose> current_point;
  current_point.setData(plan_to_global_transform * tf_front_point);
  current_point.stamp_ = plan_to_global_transform.stamp_;
  current_point.frame_id_ = global_frame_;

  // we assume the global goal is the last point in the global plan
  double goal_x = goal_point.getOrigin().getX();
  double goal_y = goal_point.getOrigin().getY();

  double yaw = tf::getYaw(goal_point.getRotation());

  double goal_th = yaw;

  // check to see if we've reached the goal position
  if (xy_tolerance_latch_ || ((getGoalPositionDistance(global_pose, goal_x, goal_y) <= xy_goal_tolerance_) && global_plan_.size() <= 100)) {
    // if the user wants to latch goal tolerance, if we ever reach the goal location, we'll
    // just rotate in place
    if (latch_xy_goal_tolerance_) {
      xy_tolerance_latch_ = true;
    }

    double angle = getGoalOrientationAngleDifference(global_pose, goal_th);
    GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] global_goal: yaw_goal_tolerance = %lf, yaw_goal_diff = %lf", yaw_goal_tolerance_, angle);
    // check to see if the goal orientation has been reached
    if (fabs(angle) <= yaw_goal_tolerance_) {
      // set the velocity command to zero
      cmd_vel->linear.x = 0.0;
      cmd_vel->linear.y = 0.0;
      cmd_vel->angular.z = 0.0;
      rotating_to_goal_ = false;
      xy_tolerance_latch_ = false;
      reached_goal_ = true;
      rotating_to_goal_done_ = true;
      GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] global_goal reached!");
    } else {
      // we need to call the next two lines to make sure that the trajectory
      // planner updates its path distance and goal distance grids
      if (planner_type == TRAJECTORY_PLANNER) {
        tc_->UpdateGoalAndPlan(goal_pose, transformed_plan);
      } else if (planner_type == LOOKAHEAD_PLANNER) {
        la_->UpdatePlan(transformed_plan);
      }
      std::vector<Trajectory> all_explored;
      double traj_vel = fixpattern_path_.front().max_vel;
      double highlight = fixpattern_path_.front().highlight;
      double current_point_dis = getGoalPositionDistance(global_pose, current_point.getOrigin().getX(), current_point.getOrigin().getY());
      Trajectory path;
      if (planner_type == TRAJECTORY_PLANNER) {
        path = tc_->findBestPath(global_pose, traj_vel, highlight, current_point_dis,
                                            robot_vel, drive_cmds, &all_explored);
      } else if (planner_type == LOOKAHEAD_PLANNER) {
        path = la_->GeneratePath(global_pose, robot_vel, traj_vel, highlight, &drive_cmds);
      }
      is_footprint_safe_ = path.is_footprint_safe_;

      // copy over the odometry information
      nav_msgs::Odometry base_odom;
      odom_helper_.getOdom(base_odom);

      // if we're not stopped yet... we want to stop... taking into account the acceleration limits of the robot
      if (!rotating_to_goal_ && !fixpattern_local_planner::stopped(base_odom, 0.1, 0.1)) { //trans_stopped_velocity_  rot_stopped_velocity_
        if (!stopWithAccLimits(planner_type, global_pose, robot_vel, cmd_vel)) {
          GAUSSIAN_ERROR("[FIXPATTERN LOCAL PLANNER] stopWithAccLimits failed");
          return false;
        }
      } else {
        // if we're stopped... then we want to rotate to goal
        // set this so that we know its OK to be moving
        rotating_to_goal_done_ = false;
        rotating_to_goal_ = true;
        if (!rotateToGoal(planner_type, global_pose, robot_vel, goal_th, cmd_vel)) {
          GAUSSIAN_ERROR("[FIXPATTERN LOCAL PLANNER] rotateToGoal failed");
          return false;
        }
      }
    }

    // publish an empty plan because we've reached our goal position
    publishPlan(transformed_plan, g_plan_pub_);
    publishPlan(local_plan, l_plan_pub_);

    // we don't actually want to run the controller when we're just rotating to goal
    return true;
  }
  // normal path trajectory
  if (planner_type == TRAJECTORY_PLANNER) {
    tc_->UpdateGoalAndPlan(goal_pose, transformed_plan);
  } else if (planner_type == LOOKAHEAD_PLANNER) {
    la_->UpdatePlan(transformed_plan);
  }

  // compute what trajectory to drive along
  std::vector<Trajectory> all_explored;
  double traj_vel = fixpattern_path_.front().max_vel;
  double highlight = fixpattern_path_.front().highlight;
  double current_point_dis = getGoalPositionDistance(global_pose, current_point.getOrigin().getX(), current_point.getOrigin().getY());
  GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] path_front.max_vel = %lf, hightlight = %lf, current_ponit_dis = %lf", traj_vel, highlight, current_point_dis);
  Trajectory path;
  if (planner_type == TRAJECTORY_PLANNER) {
    path = tc_->findBestPath(global_pose, traj_vel, highlight, current_point_dis, robot_vel, drive_cmds, &all_explored);
  } else if (planner_type == LOOKAHEAD_PLANNER) {
    path = la_->GeneratePath(global_pose, robot_vel, traj_vel, highlight, &drive_cmds);
  }
  is_footprint_safe_ = path.is_footprint_safe_;

  /* For timing uncomment
     gettimeofday(&end, NULL);
     start_t = start.tv_sec + double(start.tv_usec) / 1e6;
     end_t = end.tv_sec + double(end.tv_usec) / 1e6;
     t_diff = end_t - start_t;
     GAUSSIAN_INFO("Cycle time: %.9f", t_diff);
 */

  // GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] fixpattern_path_.size(): %d", fixpattern_path_.size());
  for(unsigned int i = 0; i < fixpattern_path_.size(); ++i) {
    if(fixpattern_path_.at(i).IsCornerPoint()) {
      GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] fixpattern_path_size = %d, corner_index = %d", (int)fixpattern_path_.size(), i);
    }
  }
  if (fixpattern_path_.front().IsCornerPoint()) {
/*
    if (needBackward(planner_type, global_pose, robot_vel, cmd_vel)) {
      publishPlan(transformed_plan, g_plan_pub_);
      publishPlan(local_plan, l_plan_pub_);
      // we don't actually want to run the controller when we're just moving backward
      return true;
    }
*/
    double yaw = tf::getYaw(global_pose.getRotation());
    double target_yaw = fixpattern_path_.front().corner_struct.theta_out;
    double angle_diff = angles::shortest_angular_distance(yaw, target_yaw);
    GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] Corner: before rotating to goal, yaw: %lf, target_yaw: %lf, angle_diff: %lf", yaw, target_yaw, angle_diff);
    // if target_yaw changed during rotation, don't follow last dir
    if (fabs(target_yaw - last_target_yaw_) > 0.000001) {
      last_rotate_to_goal_dir_ = 0;
      try_rotate_ = 0;
      last_target_yaw_ = target_yaw;
    }
    if (fabs(angle_diff) > 0.1) {
      rotating_to_goal_ = true;
      rotating_to_goal_done_ = false;
      if (!rotateToGoal(planner_type, global_pose, robot_vel, target_yaw, cmd_vel)) {
        GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] try_rotate_: %d", try_rotate_);
        return false;
      }
      GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] rotating to goal");

      publishPlan(transformed_plan, g_plan_pub_);
      publishPlan(local_plan, l_plan_pub_);

      // we don't actually want to run the controller when we're just rotating to goal
      return true;
    } else {
      rotating_to_goal_ = false;
      rotating_to_goal_done_ = true;
    }
  }/* else {
		double yaw = tf::getYaw(global_pose.getRotation());
		double acc_dis = 0.0;
		unsigned int index = 1;
		for (; index < fixpattern_path_.size() - 2; index++) {
			acc_dis += fixpattern_path_.at(index).DistanceToPoint(fixpattern_path_.at(index - 1));
			//acc_dis += getPoseDistance(transformed_plan.at(index), tranformed_plan.at(index + 1));
			if (acc_dis > 0.1) break;
		}
		double target_yaw = fixpattern_path::CalculateDirection(fixpattern_path_.front(), fixpattern_path_.at(index));
		double angle_diff = angles::shortest_angular_distance(yaw, target_yaw);
		if (robot_vel.getOrigin().getX() < GS_DOUBLE_PRECISION && tf::getYaw(robot_vel.getRotation()) < GS_DOUBLE_PRECISION && fabs(angle_diff) > 0.5) {  // > 30 digree{
        need_rotate_to_path_ = true;
        GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] start point rotating to path, yaw: %lf, target_yaw: %lf, angle_diff: %lf", yaw, target_yaw, angle_diff);
    }
    if (fabs(angle_diff) > 0.1 && need_rotate_to_path_) {
				GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] rotating to path goal");
				if (!rotateToGoal(planner_type, global_pose, robot_vel, target_yaw, cmd_vel)) {
					GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] try_rotate_: %d", try_rotate_);
					return false;
				}
				publishPlan(transformed_plan, g_plan_pub_);
				publishPlan(local_plan, l_plan_pub_);
				// we don't actually want to run the controller when we're just rotating to goal
				return true;
		} else {
      need_rotate_to_path_ = false;
    }
  }
*/
  last_target_yaw_ = 0.0;
  last_rotate_to_goal_dir_ = 0;
  try_rotate_ = 0;

  if (fixpattern_path_.front().IsCornerPoint()) {
    GAUSSIAN_INFO("[FIXPATTERN LOCAL PLANNER] path front is corner, highlight: %lf", fixpattern_path_.front().highlight);
  }

  // publish point cloud for debug
/*
  pcl::PointCloud<MapGridCostPoint> traj_cloud;
  traj_cloud.header.frame_id = global_frame_;
  MapGridCostPoint pt;
  traj_cloud.points.clear();
  traj_cloud.width = 0;
  traj_cloud.height = 0;
  std_msgs::Header header;
  pcl_conversions::fromPCL(traj_cloud.header, header);
  header.stamp = ros::Time::now();
  traj_cloud.header = pcl_conversions::toPCL(header);
  for (std::vector<Trajectory>::iterator t = all_explored.begin(); t != all_explored.end(); ++t) {
    if (t->cost_ < 0) continue;
    // Fill out the plan
    for (unsigned int i = 0; i < t->getPointsSize(); ++i) {
      double p_x, p_y, p_th;
      t->getPoint(i, p_x, p_y, p_th);
      pt.x = p_x;
      pt.y = p_y;
      pt.z = 0;
      pt.path_cost = p_th;
      pt.total_cost = t->cost_;
      traj_cloud.push_back(pt);
    }
  }
  traj_cloud_pub_.publish(traj_cloud);
*/
  // pass along drive commands
  cmd_vel->linear.x = drive_cmds.getOrigin().getX();
  cmd_vel->linear.y = drive_cmds.getOrigin().getY();
  cmd_vel->angular.z = tf::getYaw(drive_cmds.getRotation());
/*
  if ((getGoalPositionDistance(global_pose, goal_x, goal_y) <= 0.20) &&
       cmd_vel->linear.x > 0.05 && fabs(cmd_vel->angular.z > 0.10)) {
    cmd_vel->angular.z = cmd_vel->angular.z > 0.0 ? 0.10 : -0.10;
  }
*/
  // if we cannot move... tell someone
  if (path.cost_ < 0) {
    ROS_DEBUG_NAMED("trajectory_planner_ros",
                    "The rollout planner failed to find a valid plan. This means that the footprint of the robot was in collision for all simulated trajectories.");
    local_plan.clear();
    publishPlan(transformed_plan, g_plan_pub_);
    publishPlan(local_plan, l_plan_pub_);
    GAUSSIAN_ERROR("[FIXPATTERN LOCAL PLANNER] path.cost < 0");
    return false;
  }

  ROS_DEBUG_NAMED("trajectory_planner_ros", "A valid velocity command of (%.2f, %.2f, %.2f) was found for this cycle.",
                  cmd_vel->linear.x, cmd_vel->linear.y, cmd_vel->angular.z);

  // Fill out the local plan
  for (unsigned int i = 0; i < path.getPointsSize(); ++i) {
    double p_x, p_y, p_th;
    path.getPoint(i, p_x, p_y, p_th);
    tf::Stamped<tf::Pose> p =
        tf::Stamped<tf::Pose>(tf::Pose(
                tf::createQuaternionFromYaw(p_th),
                tf::Point(p_x, p_y, 0.0)),
            ros::Time::now(),
            global_frame_);
    geometry_msgs::PoseStamped pose;
    tf::poseStampedTFToMsg(p, pose);
    local_plan.push_back(pose);
  }

  // publish information to the visualizer
  publishPlan(transformed_plan, g_plan_pub_);
  publishPlan(local_plan, l_plan_pub_);
  return true;
}

bool FixPatternTrajectoryPlannerROS::isGoalReached() {
  if (!isInitialized()) {
    GAUSSIAN_ERROR("This planner has not been initialized, please call initialize() before using this planner");
    return false;
  }
  // return flag set in controller
  return reached_goal_;
}

bool FixPatternTrajectoryPlannerROS::isRotatingToGoalDone() {
  if (!isInitialized()) {
    GAUSSIAN_ERROR("This planner has not been initialized, please call initialize() before using this planner");
    return false;
  }
  // return flag set in controller
  return rotating_to_goal_done_;
}

void FixPatternTrajectoryPlannerROS::resetRotatingToGoalDone() {
  // return flag set in controller
  rotating_to_goal_done_ = false;
}


bool FixPatternTrajectoryPlannerROS::isRotatingToGoal() {
  if (!isInitialized()) {
    GAUSSIAN_ERROR("This planner has not been initialized, please call initialize() before using this planner");
    return false;
  }
  // return flag set in controller
  return rotating_to_goal_;
}

bool FixPatternTrajectoryPlannerROS::isGoalXYLatched() {
  if (!isInitialized()) {
    GAUSSIAN_ERROR("This planner has not been initialized, please call initialize() before using this planner");
    return false;
  }
  // return flag set in controller
  return xy_tolerance_latch_;
}

void FixPatternTrajectoryPlannerROS::reset_planner() {
   reached_goal_ = false;
   rotating_to_goal_done_ = false;
   xy_tolerance_latch_ = false;
   final_goal_extended_ = false;

   last_target_yaw_ = 0.0;
   last_rotate_to_goal_dir_ = 0;
   try_rotate_ = 0;
}

};  // namespace fixpattern_local_planner
