/* Copyright(C) Gaussian Robot. All rights reserved.
 */

/**
 * @file astar_controller.cpp
 * @brief implementation of astar controller
 * @author cameron<chenkan@gs-robot.com>
 * @version 1.0.0.0
 * @date 2015-08-21
 */

#include "astar_controller.h"
#include "bezier_planner.h"
#include <nav_msgs/Path.h>
#include <angles/angles.h>
#include <std_msgs/UInt32.h>

namespace service_robot {

AStarController::AStarController(tf::TransformListener* tf,
                                 costmap_2d::Costmap2DROS* controller_costmap_ros)
    : tf_(*tf),
      controller_costmap_ros_(controller_costmap_ros), planner_plan_(NULL), 
      planner_goal_index_(0), sbpl_reached_goal_(false), 
      runPlanner_(false), new_global_plan_(false), first_run_controller_flag_(true), gotInitPlan_(false),
      using_sbpl_directly_(false), sbpl_broader_(false), last_using_bezier_(false), replan_directly_(false),
      astar_planner_timeout_cnt_(0), local_planner_timeout_cnt_(0), fix_local_planner_error_cnt_(0),
      goal_not_safe_cnt_(0), path_not_safe_cnt_(0) {
  // set up plan triple buffer
  planner_plan_ = new std::vector<geometry_msgs::PoseStamped>();

  // create footprint_checker_
  footprint_checker_ = new service_robot::FootprintChecker(controller_costmap_ros_->getCostmap());

  footprint_spec_ = controller_costmap_ros_->getRobotFootprint();
  unpadded_footrpint_spec_ = controller_costmap_ros_->getUnpaddedRobotFootprint();

  costmap_2d::calculateMinAndMaxDistances(footprint_spec_, inscribed_radius_, circumscribed_radius_);
  // set up the planner's thread
  planner_thread_ = new boost::thread(boost::bind(&AStarController::PlanThread, this));

  // initially, we'll need to make a plan
  state_ = A_PLANNING;
  
  // disable localization_recovery_ when start	
  localization_valid_ = false;
  // we'll start executing recovery behaviors at the beginning of our list
  recovery_trigger_ = A_PLANNING_R;

  using_static_costmap_ = false;
  switch_path_ = false;
  origin_path_safe_cnt_ = 0;
  // set rotate_recovery_dir_
  rotate_recovery_dir_ = 0;
  rotate_failure_times_ = 0;
  try_recovery_times_ = 0;
  cmd_vel_ratio_ = 1.0;

  // set for fixpattern_path
  ros::NodeHandle fixpattern_nh("~/fixpattern_global_planner");
  fixpattern_pub_ = fixpattern_nh.advertise<nav_msgs::Path>("plan", 1);
  ros::NodeHandle n;
  ros::NodeHandle device_nh("device");
  move_base_status_pub_ = n.advertise<std_msgs::UInt32>("move_base_status", 10);
  goal_reached_pub_ = n.advertise<geometry_msgs::PoseStamped>("goal_reached", 10);
  heading_goal_pub_ = n.advertise<geometry_msgs::PoseStamped>("heading_goal", 10);
  init_finished_pub_ = n.advertise<geometry_msgs::PoseStamped>("init_finished", 10);
  astar_goal_pub_ = n.advertise<geometry_msgs::PoseStamped>("a_goal", 10);
  astar_start_pub_ = n.advertise<geometry_msgs::PoseStamped>("a_start", 10);
  sbpl_goal_pub_ = n.advertise<geometry_msgs::PoseStamped>("s_temp_goal", 10);
  astar_extend_pose_pub_ = n.advertise<geometry_msgs::PoseStamped>("a_extend_pose", 10);

  localization_sub_ = n.subscribe<std_msgs::Int8>("/localization_bit", 100, boost::bind(&AStarController::LocalizationCallBack, this, _1));

  start_rotate_client_ = device_nh.serviceClient<autoscrubber_services::StartRotate>("start_rotate");
  stop_rotate_client_ = device_nh.serviceClient<autoscrubber_services::StopRotate>("stop_rotate");
  check_rotate_client_ = device_nh.serviceClient<autoscrubber_services::CheckRotate>("check_rotate");
  check_protector_client_ = device_nh.serviceClient<autoscrubber_services::CheckProtectorStatus>("check_protector_status");
  check_goal_srv_ = n.advertiseService("check_goal", &AStarController::CheckGoalIsSafe, this);
}

AStarController::~AStarController() {
  planner_thread_->interrupt();
  planner_thread_->join();

  delete footprint_checker_;

  delete planner_thread_;

  delete planner_plan_;
}

void AStarController::LocalizationCallBack(const std_msgs::Int8::ConstPtr& param) { 
  if (param->data == 0) {
    localization_valid_ = true;
//    GAUSSIAN_WARN("[ASTAR CONTROLLER] localization success!"); 
  } else {
    localization_valid_ = false;
//    GAUSSIAN_WARN("[ASTAR CONTROLLER] localization failed!"); 
  }
}

bool AStarController::CheckGoalIsSafe(autoscrubber_services::CheckGoal::Request& req, autoscrubber_services::CheckGoal::Response& res) {
  geometry_msgs::PoseStamped goal_pose = req.goal_pose; 
  footprint_checker_->setStaticCostmap(controller_costmap_ros_, false);
  res.isSafe = IsGoalSafe(goal_pose, 0.15, 0.15);
  return true;
}

bool AStarController::MakePlan(const geometry_msgs::PoseStamped& start, const geometry_msgs::PoseStamped& goal, std::vector<geometry_msgs::PoseStamped>* plan) {

  // make sure to set the plan to be empty initially
  plan->clear();

  // since this gets called on handle activate
  if (controller_costmap_ros_ == NULL) {
    GAUSSIAN_ERROR("Planner costmap ROS is NULL, unable to create global plan");
    return false;
  }
  replan_directly_ = false;

  if (PoseStampedDistance(start, goal) <= 0.25) {
    // set this to true as we'll use it afterwards
    using_sbpl_directly_ = true;
    last_using_bezier_ = false;

    GAUSSIAN_INFO("[ASTAR PLANNER] too short, take start and goal as plan directly");
    // too short, plan direct path
    plan->clear();
    plan->push_back(start);
    plan->push_back(goal);

    // assign to astar_path_
    std::vector<fixpattern_path::PathPoint> path;
    for (const auto& p : *plan) {
      path.push_back(fixpattern_path::GeometryPoseToPathPoint(p.pose));
    }
    astar_path_.set_short_sbpl_path(start, path);
  } else if (!last_using_bezier_ && PoseStampedDistance(start, goal) <= 2.0) {
    GAUSSIAN_INFO("[ASTAR PLANNER] use bezier planner");
    using_sbpl_directly_ = true;
    last_using_bezier_ = true;

    // get path limited length
    std::vector<fixpattern_path::PathPoint> bezier_path;
    if (!MakeBezierPlan(&bezier_path, start, goal, true) || bezier_path.size() == 0) {
      GAUSSIAN_WARN("[ASTAR PLANNER] bezier failed to find a plan, replan directly");
      replan_directly_ = true;
      return false;
    }
    astar_path_.set_bezier_path(start, bezier_path, state_ == A_PLANNING);
    // if invalid, return false
    if (!IsPathFootprintSafe(astar_path_, co_->front_safe_check_dis)) {
      GAUSSIAN_INFO("[ASTAR PLANNER] bezier_path not safe, replan directly");
      replan_directly_ = true;
      return false;
    }
/*
    // don't forget to fill plan
    ros::Time now = ros::Time::now();
    plan->push_back(start);
    for (const auto& p : bezier_path) {
      geometry_msgs::PoseStamped pp;
      pp.pose = p;
      pp.header.stamp = now;
      pp.header.frame_id = co_->global_frame;
      plan->push_back(pp);
    }
    plan->push_back(planner_goal_);
*/
  } else if (PoseStampedDistance(start, goal) <= co_->sbpl_max_distance) {
    // too short, use sbpl directly
    GAUSSIAN_INFO("[ASTAR PLANNER] use sbpl directly");
    using_sbpl_directly_ = true;
    last_using_bezier_ = false;
    // if last plan was success due to sbpl_broader_ and goal has not changed,
    // we'll still use sbpl_broader_
//    bool sbpl_broader = sbpl_broader_;
//    if (PoseStampedDistance(goal, success_broader_goal_) < GS_DOUBLE_PRECISION)
//      sbpl_broader = true;
    // set static costmap if first planning
    co_->sbpl_global_planner->setStaticCosmap(using_static_costmap_);

    // if the planner fails or returns a zero length plan, planning failed
    if (!co_->sbpl_global_planner->makePlan(start, goal, *plan, astar_path_, sbpl_broader_, state_ != A_PLANNING) || plan->empty()) {
      GAUSSIAN_ERROR("[ASTAR PLANNER] s_planner failed to find a plan to point (%.2f, %.2f)", goal.pose.position.x, goal.pose.position.y);
      return false;
    } else {
      gotInitPlan_ = true;
      GAUSSIAN_INFO("[ASTAR PLANNER] got s path size = %zu, path len = %lf", astar_path_.path().size(), astar_path_.Length());
    }
  } else {
    // astar plan, it needs call set_fix_path to generate astar_path_
    GAUSSIAN_INFO("[ASTAR PLANNER] take a_planner");

    using_sbpl_directly_ = false;
    last_using_bezier_ = false;
    // set static costmap if first planning
    co_->astar_global_planner->setStaticCosmap(using_static_costmap_);
    if (!co_->astar_global_planner->makePlan(start, goal, *plan) || plan->empty()) {
      GAUSSIAN_ERROR("[ASTAR PLANNER] a_planner failed to find a plan to point (%.2f, %.2f)", global_goal_.pose.position.x, global_goal_.pose.position.y);
      PublishAStarExtendPose(start, goal);
      return false;
    }

    // assign to astar_path_
    std::vector<fixpattern_path::PathPoint> path;
    SampleInitialPath(plan, path);
    astar_path_.set_fix_path(start, path, true); 
    GAUSSIAN_INFO("[ASTAR PLANNER] got a path size = %zu, path len = %lf", path.size(), astar_path_.Length());

    std::vector<geometry_msgs::PoseStamped> a_plan = astar_path_.GeometryPath();
    for (auto&& p : a_plan) {  // NOLINT
      p.header.frame_id = co_->global_frame;
      p.header.stamp = ros::Time::now();
    }
    PublishPlan(fixpattern_pub_, a_plan);
    
    return true;
/*
    // get sbpl goal and goal direction
    size_t i = 0;
    double dis_accu = 0.0;
    for (; i < plan->size() - 1; ++i) {
      dis_accu += PoseStampedDistance(plan->at(i), plan->at(i + 1));
      if (dis_accu > co_->sbpl_max_distance)
        break;
    }
    size_t end_i = std::min(i + 3, plan->size() - 1);
    double yaw = fixpattern_path::CalculateDirection(plan->at(i).pose, plan->at(end_i).pose);
    tf::Quaternion temp;
    temp.setRPY(0, 0, yaw);
    plan->at(i).pose.orientation.x = temp.getX();
    plan->at(i).pose.orientation.y = temp.getY();
    plan->at(i).pose.orientation.z = temp.getZ();
    plan->at(i).pose.orientation.w = temp.getW();
    // sbpl make plan, use plan->at(i) as goal
    geometry_msgs::PoseStamped temp_goal = plan->at(i);
    // if last plan was success due to sbpl_broader_ and new goal is not far
    // away, we'll still use sbpl_broader_
    bool sbpl_broader = sbpl_broader_;
    if (PoseStampedDistance(temp_goal, success_broader_goal_) < 0.1)
      sbpl_broader = true;
    if (!co_->sbpl_global_planner->makePlan(start, temp_goal, *plan, astar_path_, sbpl_broader, state_ != A_PLANNING) || plan->empty()) {
      GAUSSIAN_ERROR("[ASTAR PLANNER] sbpl failed to find a plan to point (%.2f, %.2f)", temp_goal.pose.position.x, temp_goal.pose.position.y);
      return false;
    }
*/
  }

  return true;
}

void AStarController::PublishAStarExtendPose(const geometry_msgs::PoseStamped& start, const geometry_msgs::PoseStamped& goal) {
  double extend_x, extend_y;
  co_->astar_global_planner->getExtendPoint(extend_x, extend_y);
	geometry_msgs::PoseStamped extend_pose;
  double target_yaw = fixpattern_path::CalculateDirection(fixpattern_path::GeometryPoseToPathPoint(start.pose), fixpattern_path::GeometryPoseToPathPoint(goal.pose));
  extend_pose.header.frame_id = co_->global_frame;
  extend_pose.header.stamp = ros::Time::now();
  extend_pose.pose.position.x = extend_x;
  extend_pose.pose.position.y = extend_y;
  tf::Quaternion temp;
  temp.setRPY(0, 0, target_yaw);
  extend_pose.pose.orientation.x = temp.getX();
  extend_pose.pose.orientation.y = temp.getY();
  extend_pose.pose.orientation.z = temp.getZ();
  extend_pose.pose.orientation.w = temp.getW();
  target_yaw = fixpattern_path::CalculateDirection(fixpattern_path::GeometryPoseToPathPoint(extend_pose.pose), fixpattern_path::GeometryPoseToPathPoint(goal.pose));
  temp.setRPY(0, 0, target_yaw);
  extend_pose.pose.orientation.x = temp.getX();
  extend_pose.pose.orientation.y = temp.getY();
  extend_pose.pose.orientation.z = temp.getZ();
  extend_pose.pose.orientation.w = temp.getW();
  extend_pose.header.frame_id = co_->global_frame;
  astar_extend_pose_pub_.publish(extend_pose);
  double extend_dis = PoseStampedDistance(start, extend_pose);
  GAUSSIAN_WARN("[ASTAR CONTROLLER] InitialPath: a_planner extend dis = %lf", extend_dis); 
}

void AStarController::PublishZeroVelocity() {
  if (fabs(last_valid_cmd_vel_.linear.x) > 0.001) {
    geometry_msgs::Twist cmd_vel;
    cmd_vel_ratio_ = 1.0;
    cmd_vel.linear.x = 0.0;
    cmd_vel.linear.y = 0.0;
    cmd_vel.angular.z = 0.0;
    co_->vel_pub->publish(cmd_vel);
    last_valid_cmd_vel_ = cmd_vel;
    GAUSSIAN_WARN("[ASTAR CONTROLLER] Publish Zero Velocity!");
  }
}

void AStarController::PublishVelWithAcc(geometry_msgs::Twist last_cmd_vel, double vel_acc) {
   GAUSSIAN_INFO("[ASTAR CONTROLLER] Publish Velocity with acc = %lf", vel_acc);
  if (fabs(last_valid_cmd_vel_.linear.x) > 0.001) {
    geometry_msgs::Twist cmd_vel = last_valid_cmd_vel_;
    cmd_vel.linear.y = 0.0;
    cmd_vel.angular.z = 0.0;
    ros::Rate r(10);
    while(fabs(cmd_vel.linear.x) > 0.001 && CanForward(0.05) && env_->run_flag) {
      cmd_vel.linear.x = cmd_vel.linear.x - vel_acc < 0.05 ? 0.0 : cmd_vel.linear.x - vel_acc;
      co_->vel_pub->publish(cmd_vel);
      r.sleep();
    }
    PublishZeroVelocity();
  }
}

void AStarController::WakePlanner(const ros::TimerEvent& event) {
  // we have slept long enough for rate
  planner_cond_.notify_one();
}

double GetTimeInSeconds() {
  timeval t;
  gettimeofday(&t, NULL);
  return t.tv_sec + 0.000001 * t.tv_usec;
}

void AStarController::PlanThread() {
  GAUSSIAN_INFO("[ASTAR PLANNER] Starting planner thread...");
  ros::NodeHandle n;
  ros::Timer timer;
  bool wait_for_wake = false;
  boost::unique_lock<boost::mutex> lock(planner_mutex_);
  double start_t;
  while (n.ok()) {
    // check if we should run the planner (the mutex is locked)
    while (wait_for_wake || !runPlanner_) {
      // if we should not be running the planner then suspend this thread
      ROS_DEBUG_NAMED("move_base_plan_thread", "Planner thread is suspending");
      planner_cond_.wait(lock);
      wait_for_wake = false;
      start_t = GetTimeInSeconds();
      last_valid_plan_ = ros::Time::now();
    }

    if (!gotInitPlan_ && astar_planner_timeout_cnt_ < 1) {
      using_static_costmap_ = true;
    } else {
      using_static_costmap_ = false;
      footprint_checker_->setStaticCostmap(controller_costmap_ros_, false);
    }
    GAUSSIAN_INFO("[ASTAR PLANNER] Plan Start!");
    ros::Time start_time = ros::Time::now();
//    controller_costmap_ros_->getCostmap();
 
    // time to plan! get a copy of the goal and unlock the mutex
    geometry_msgs::PoseStamped temp_goal = planner_goal_;
    lock.unlock();
    ROS_DEBUG_NAMED("move_base_plan_thread", "Planning...");

    // get the starting pose of the robot
    geometry_msgs::PoseStamped start;
    tf::Stamped<tf::Pose> global_pose;
    bool gotStartPose = true;
    bool gotPlan = false;
    if (!controller_costmap_ros_->getRobotPose(global_pose)) {
      gotStartPose = false;
      GAUSSIAN_ERROR("[ASTAR PLANNER]Unable to get starting pose of robot, unable to create global plan");
    } else {
      tf::poseStampedTFToMsg(global_pose, start);
      start.header.frame_id = co_->global_frame;
    }
    if (state_ == FIX_CONTROLLING) {
			if(planning_state_ == P_INSERTING_MIDDLE) {
        if (!GetAStarStart(co_->front_safe_check_dis, 0.0, 0.0)) {
          GAUSSIAN_WARN("[ASTAR PLANNER]Unable to get AStar start, take current pose in place, and planning_state_ = BEGIN ");
          planning_state_ = P_INSERTING_BEGIN;
        } else {
          start = planner_start_;
        }
      } else if(planning_state_ == P_INSERTING_SBPL) {
        start = sbpl_planner_goal_;
        GetAStarTempGoal(sbpl_planner_goal_, co_->sbpl_max_distance - 0.5);
        temp_goal = sbpl_planner_goal_;
      }
    }

    planner_start_ = start;
    if(gotStartPose) {
      // run planner
			if (state_ == A_PLANNING) {
        PublishMovebaseStatus(I_GOAL_PLANNING);
      }
      planner_plan_->clear();
      gotPlan = n.ok() && MakePlan(start, temp_goal, planner_plan_) && !astar_path_.path().empty();
      if (replan_directly_) {
        // bezier failed, just replan
        replan_directly_ = false;
        gotPlan = n.ok() && MakePlan(start, temp_goal, planner_plan_) && !astar_path_.path().empty();
      }
    }

    if (gotPlan) {
      GAUSSIAN_INFO("[ASTAR PLANNER] Got Plan with %zu points! cost: %lf secs", planner_plan_->size(), GetTimeInSeconds() - start_t);
      // check distance from current pose to the path.front() 
      geometry_msgs::PoseStamped cur_pos;
      controller_costmap_ros_->getRobotPose(global_pose);
      tf::poseStampedTFToMsg(global_pose, cur_pos);
      double distance_diff = PoseStampedDistance(cur_pos, astar_path_.GeometryPath().front());
      if (distance_diff > 0.3 && state_ == A_PLANNING) {
        GAUSSIAN_WARN("[ASTAR PLANNER] Distance from start to path_front = %lf > 0.3m, continue", distance_diff);
      } else {
        last_valid_plan_ = ros::Time::now();
        new_global_plan_ = true;
        // reset rotate_recovery_dir_
        rotate_recovery_dir_ = 0;
        rotate_failure_times_ = 0;
        try_recovery_times_ = 0;
        astar_planner_timeout_cnt_ = 0;
        lock.lock();
        front_path_.set_path(co_->fixpattern_path->path(), false, false);
        front_goal_ = temp_goal;
        // TODO(lizhen) final path but middle state?
        if (taken_global_goal_ || planning_state_ == P_INSERTING_NONE) {
          if (using_sbpl_directly_) {
            co_->fixpattern_path->set_sbpl_path(start, astar_path_.path(), true);
            gotInitPlan_ = true;
          } else {
            co_->fixpattern_path->set_path(astar_path_.path(), false, false);
            // check fix_path is safe: if not, get goal on path and switch to PLANNING state 
            if (RecheckFixPath(start, using_static_costmap_)) {
              GAUSSIAN_INFO("[ASTAR CONTROLLER] recheck fixpath successed!");
            } else {
              GAUSSIAN_WARN("[ASTAR CONTROLLER] recheck fixpath failed!");
            }
          }
          taken_global_goal_ = false;
          gotInitPlan_ = true;
          first_run_controller_flag_ = true;
          switch_path_ = true;
          origin_path_safe_cnt_ = 0;
          footprint_checker_->setStaticCostmap(controller_costmap_ros_, false);
        } else if (planning_state_ == P_INSERTING_BEGIN) {
          double corner_yaw_diff = state_ == A_PLANNING ? M_PI / 36.0 : M_PI / 3.0;
          co_->fixpattern_path->insert_begin_path(astar_path_.path(), start, temp_goal, false, corner_yaw_diff, using_sbpl_directly_);
          first_run_controller_flag_ = true;
          switch_path_ = true;
          origin_path_safe_cnt_ = 0;
        } else if (planning_state_ == P_INSERTING_END) {
          co_->fixpattern_path->insert_end_path(astar_path_.path());
          first_run_controller_flag_ = true;
        } else if (planning_state_ == P_INSERTING_MIDDLE) {
          co_->fixpattern_path->insert_middle_path(astar_path_.path(), start, temp_goal);
          front_safe_check_cnt_ = 0; // only set 0 after getting new fix_path
          switch_path_ = true;
          origin_path_safe_cnt_ = 0;
          // first_run_controller_flag_ = true;
        } else if (planning_state_ == P_INSERTING_SBPL) {
          // co_->fixpattern_path->insert_middle_path(astar_path_.path(), start, temp_goal);
          // first_run_controller_flag_ = true;
        } else { // unkonw state
          // switch to FIX_CLEARING state
          gotPlan = false;
          runPlanner_ = false;
          switch_path_ = false;
          state_ = FIX_CLEARING;
          recovery_trigger_ = GLOBAL_PLANNER_RECOVERY_R;
          GAUSSIAN_ERROR("[ASTAR CONTROLLER] planning_state_ unknown, enter recovery");
        }

        if (gotPlan) {
          double path_length_diff = co_->fixpattern_path->Length() - front_path_.Length();
          GAUSSIAN_WARN("[ASTAR CONTROLLER] new plan - pre plan length = %lf, max_path_length_diff = %lf", path_length_diff, co_->max_path_length_diff);
          if (front_path_.Length() > 0.5 && path_length_diff > co_->max_path_length_diff) {
            if (co_->use_farther_planner) {
              planner_goal_ = global_goal_;
              taken_global_goal_ = true;
              new_global_plan_ = false;
              // runPlanner_ = true;
              state_ = A_PLANNING;
              GAUSSIAN_WARN("[ASTAR CONTROLLER] getting farther path, taking global goal as astar_goal_ and replan!");
            } else {
              ++astar_planner_timeout_cnt_;
              gotPlan = false;
              runPlanner_ = false;
              switch_path_ = false;
              state_ = FIX_CLEARING;
              recovery_trigger_ = GLOBAL_PLANNER_RECOVERY_R;
              GAUSSIAN_ERROR("[ASTAR CONTROLLER] getting farther path, switch to GLOBAL_PLANNER_RECOVERY_R");
            }
          } else {
            runPlanner_ = false;
            state_ = FIX_CONTROLLING;
          }
        } 
        lock.unlock();
      }
    } else if (state_ == A_PLANNING) {  // if we didn't get a plan and we are in the planning state (the robot isn't moving)
      GAUSSIAN_ERROR("[ASTAR PLANNER] No Plan...");
//      sbpl_broader_ = true;
      ros::Time attempt_end = last_valid_plan_ + ros::Duration(co_->planner_patience);
      // check if we've tried to make a plan for over our time limit
      lock.lock();
      if (ros::Time::now() > attempt_end && runPlanner_) {
        // don't allow plan, as RotateRecovery needs global costmap
        runPlanner_ = false;
        PublishZeroVelocity();
        // switch to FIX_CLEARING state
        state_ = FIX_CLEARING;
        recovery_trigger_ = GLOBAL_PLANNER_RECOVERY_R;
        planning_state_ = P_INSERTING_BEGIN;
        ++astar_planner_timeout_cnt_;
        GAUSSIAN_ERROR("[ASTAR PLANNER] Alarm Here!!! Not got plan until planner_patience, enter recovery; timeout_cnt = %d", astar_planner_timeout_cnt_);
        if (!gotInitPlan_ && astar_planner_timeout_cnt_ > 4) {
          PublishMovebaseStatus(I_GOAL_UNREACHABLE);
          env_->run_flag = false;
          env_->pause_flag = false;
          GAUSSIAN_ERROR("[ASTAR CONTROLLER] planner_timeout_cnt_ > 3, set run_flag false and return here!");
        }
      } else if (runPlanner_) {
        // to update global costmap
        usleep(500000);
//        GetAStarGoal(start, 0.0, 0.0);
      }
      lock.unlock();
//    } else if (state_ == FIX_CONTROLLING && planning_state_ == P_INSERTING_MIDDLE) { 
    } else if (state_ == FIX_CONTROLLING) { 
      GAUSSIAN_WARN("[ASTAR PLANNER] Plan middle path failed, just return!");
      lock.lock();
      runPlanner_ = false;
      front_safe_check_cnt_ = 0; // only set 0 after getting new fix_path
      state_ = FIX_CONTROLLING;
      lock.unlock();
    }
    // take the mutex for the next iteration
    lock.lock();

    // setup sleep interface if needed
    if (co_->planner_frequency > 0) {
      ros::Duration sleep_time = (start_time + ros::Duration(1.0 / co_->planner_frequency)) - ros::Time::now();
      if (sleep_time > ros::Duration(0.0)) {
        wait_for_wake = true;
        timer = n.createTimer(sleep_time, &AStarController::WakePlanner, this);
      }
    }
    GAUSSIAN_INFO("[ASTAR PLANNER] Plan End!");
  }
}

bool AStarController::Control(BaseControlOption* option, ControlEnvironment* environment) {
  GAUSSIAN_INFO("[ASTAR CONTROLLER] Switch to Astar Controller!");
  co_ = reinterpret_cast<AStarControlOption*>(option);
  env_ = environment;
  ros::NodeHandle n;
  while (n.ok()) {
    if (!env_->run_flag) {
      usleep(50000);
      continue;
    }

    GAUSSIAN_INFO("[ASTAR CONTROLLER] start to handle goal!");
   
    global_goal_.header.frame_id = co_->global_frame;
    global_goal_type_ = co_->movebase_goal->type;
    if (global_goal_type_ == NORMAL || global_goal_type_ == ORIGIN) {
      global_goal_ = co_->movebase_goal->pose; 
    } else if (global_goal_type_ == CHARGING) {
      co_->fixpattern_local_planner->setGoalTolerance(0.05, 0.05);
      charging_goal_ = co_->movebase_goal->pose; 
      double yaw = tf::getYaw(charging_goal_.pose.orientation);
      global_goal_.pose.position.x = charging_goal_.pose.position.x + inscribed_radius_ * cos(yaw);
      global_goal_.pose.position.y = charging_goal_.pose.position.y + inscribed_radius_ * sin(yaw);
      global_goal_.pose.orientation = charging_goal_.pose.orientation;
    }

    sbpl_goal_pub_.publish(global_goal_);
    // 0. check if goal is safe
    // check if goal outside map or unknow area 
    GAUSSIAN_INFO("[ASTAR CONTROLLER] Start to handle goal!");
    if (IsGoalUnreachable(global_goal_)) {
      PublishMovebaseStatus(I_GOAL_UNREACHABLE);
      env_->run_flag = false;
      env_->pause_flag = false;
      GAUSSIAN_ERROR("[ASTAR CONTROLLER] checking global_goal unknown or outside of map, just return here!");
      continue; 
    }
    GAUSSIAN_INFO("[ASTAR CONTROLLER] checking global goal is reachable");

    // clear footprint on normal and static costmap
    ClearFootprintInCostmap(global_goal_, 0.15, true);
    // check if goal safe on normal and static costmap
    if (!IsGoalSafe(global_goal_, 0.10, 0.10, true) && !IsGoalSafe(global_goal_, 0.10, 0.10, false)) {
      PublishMovebaseStatus(I_GOAL_UNREACHABLE);
      env_->run_flag = false;
      env_->pause_flag = false;
      GAUSSIAN_ERROR("[ASTAR CONTROLLER] checking global_goal not safe, just return here!");
      continue; 
    }
    footprint_checker_->setStaticCostmap(controller_costmap_ros_, false);

    // 1. check if location is valid
    unsigned int try_count = 0;
    while (!LocalizationRecovery() && ++try_count < 3) {
      GAUSSIAN_WARN("[ASTAR CONTROLLER] localization failed! Recovery now by inplace_rotating");
      usleep(500000);
    }
    if (try_count >= 3) {
      PublishMovebaseStatus(E_LOCATION_INVALID);
      env_->run_flag = false;
      env_->pause_flag = false;
      GAUSSIAN_ERROR("[ASTAR CONTROLLER] localization failed and try count > 3, just return here!");
      continue; 
    }
    GAUSSIAN_INFO("[ASTAR CONTROLLER] check localization successed!");
    usleep(50000);
   
    // 2. get current pose 
    double cur_goal_distance;
    controller_costmap_ros_->getCostmap(); // costmap only updated when we calling getCostmap()
    geometry_msgs::PoseStamped current_position;
    usleep(10000);
    tf::Stamped<tf::Pose> global_pose;
    if (!controller_costmap_ros_->getRobotPose(global_pose)) {
      GAUSSIAN_WARN("Unable to get starting pose of robot, unable to create sbpl plan");
      continue; 
    } else {
      tf::poseStampedTFToMsg(global_pose, current_position);
      cur_goal_distance = PoseStampedDistance(current_position, global_goal_); 
    }
    GAUSSIAN_INFO("[ASTAR CONTROLLER] distance from current pose to goal = %lf", cur_goal_distance);
		
    // 3. check if current_position and goal too close
    GAUSSIAN_INFO("[ASTAR CONTROLLER] check if current_position and goal too close!");
    if (IsGlobalGoalReached(current_position, global_goal_, 
                             co_->fixpattern_local_planner->xy_goal_tolerance_, co_->fixpattern_local_planner->yaw_goal_tolerance_)) {
      GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] current position too close to global goal, teminate controller");
      // publish goal reached 
      PublishGoalReached(global_goal_);
      PublishMovebaseStatus(I_GOAL_REACHED);
      env_->run_flag = false;
      env_->pause_flag = false;
      continue; 
    }

    // 4. check if current_position footpirnt is valid
    // if no - HandleGoingBack and Escape recovery;
	
    if (HandleGoingBack(current_position, co_->backward_check_dis + 0.05)) {
      controller_costmap_ros_->getRobotPose(global_pose);
      tf::poseStampedTFToMsg(global_pose, current_position);
      cur_goal_distance = PoseStampedDistance(current_position, global_goal_); 
    } else {
      GAUSSIAN_INFO("[FIXPATTERN CONTROLLER] front safe check ok, continue");
    } 
    if (footprint_checker_->FootprintCost(current_position, unpadded_footrpint_spec_, 0.0, 0.0) < 0.0 ||
        footprint_checker_->BroaderFootprintCost(current_position, footprint_spec_, co_->recovery_footprint_extend_x + 0.03, co_->recovery_footprint_extend_y + 0.03) < 0.0) {
       GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] footprint cost check < 0!, switch to Recovery");
       // TODO(lizhen) not terminate even EscapeRecovery failed?
       if (!EscapeRecovery(current_position)) {
         GAUSSIAN_ERROR("[FIXPATTERN CONTROLLER] footprint not safe and recovery failed, we'll not terminate and try to clear footprint!");
         ClearFootprintInCostmap(current_position, 0.05, true);
       }
    } else {
       GAUSSIAN_INFO("[FIXPATTERN CONTROLLER] broader footprint cost check ok, continue");
    } 

    gotInitPlan_ = false;
    using_static_costmap_ = true;
    planner_goal_ = global_goal_;
    taken_global_goal_ = true;
    state_ = A_PLANNING;
    planning_state_ = P_INSERTING_NONE;
    GAUSSIAN_INFO("[ASTAR CONTROLLER] taking global goal as planner_goal, and set state_ = start planning");
/*
    // check distance from goal and current pose, if < 2.5m, switch to A_PLANNING 
    if (cur_goal_distance < 2.5) { 
      GAUSSIAN_INFO("[ASTAR CONTROLLER] dis_current_goal < 2.5m and footprint check OK, take global goal as planner_goal");
      planner_goal_ = global_goal_;
      taken_global_goal_ = true;
      state_ = A_PLANNING;
      planning_state_ = P_INSERTING_NONE;
    } else {
      // firstly, get astar Path, and set as fixpattern_path 
      GAUSSIAN_INFO("[ASTAR_CONTROLLER] try to get Astar Path, and set as fixpattern_path");
      // setSaticCostmap only for inital AStar Plan 
      co_->astar_global_planner->setStaticCosmap(true);
      bool init_path_got = GetAStarInitialPath(current_position, global_goal_);
      co_->astar_global_planner->setStaticCosmap(false);
      if (init_path_got) {
        std::vector<geometry_msgs::PoseStamped> fix_path = co_->fixpattern_path->GeometryPath();
        // check fix_path is safe: if not, get astar goal on path and switch to PLANNING state 
        if (CheckFixPathFrontSafe(fix_path, co_->front_safe_check_dis, 0.0, 0.0) < 1.5) {
          if (GetAStarGoal(current_position, 0.0, 0.0, obstacle_index_)) {
            state_ = A_PLANNING;
            planning_state_ = P_INSERTING_BEGIN;
            GAUSSIAN_WARN("[ASTAR_CONTROLLER] CheckFixPathFrontSafe failed, switch to A_PLANNING state");
          } else {
            state_ = FIX_CLEARING;
            recovery_trigger_ = PLANNER_RECOVERY_R;
            GAUSSIAN_WARN("[ASTAR_CONTROLLER] get Astar goal fialed, siwtch to PLANNER_RECOVERY_R");
          }
        } else {
          // switch to CONTROLLING state and go
          state_ = FIX_CONTROLLING;
        }
      } else { 
        planner_goal_ = global_goal_;
        taken_global_goal_ = true;
        state_ = A_PLANNING;
        planning_state_ = P_INSERTING_NONE;
      }
    }
*/   
    // initialize planner and some flag
    co_->fixpattern_local_planner->reset_planner();
    first_run_controller_flag_ = true;
    using_sbpl_directly_ = false;
    last_using_bezier_ = false;
    replan_directly_ = false;
   
    // we want to make sure that we reset the last time we had a valid plan and control
    last_valid_control_ = ros::Time::now();
    last_valid_plan_ = ros::Time::now();
    last_oscillation_reset_ = ros::Time::now();
   
    ros::Rate r(co_->controller_frequency);
    while (n.ok()) {
      // if is paused but run, continue
      if (env_->pause_flag && env_->run_flag) {
        GAUSSIAN_WARN("[ASTAR CONTROLLER] Control Paused, just stop here!");
        PublishVelWithAcc(last_valid_cmd_vel_, co_->stop_to_zero_acc);
        r.sleep();
        continue;
      }
      // if terminated, break this loop directly
      if (!env_->run_flag) {
        co_->fixpattern_path->EraseToPoint(fixpattern_path::GeometryPoseToPathPoint(global_goal_.pose));
        ResetState();
   
        // disable the planner thread
        boost::unique_lock<boost::mutex> lock(planner_mutex_);
        runPlanner_ = false;
        lock.unlock();
   
        // TODO(chenkan): check if this is needed
        co_->fixpattern_local_planner->reset_planner();
   
        // we need to notify fixpattern_path
        co_->fixpattern_path->FinishPath();
        GAUSSIAN_WARN("[ASTAR CONTROLLER] Control Teminated, stop and break this loop");
        // set pause_flag = false, to let service_robot know this loop terminated
        env_->pause_flag = false;
        break;
      }
   
      // for timing that gives real time even in simulation
      ros::WallTime start = ros::WallTime::now();
   
      // the real work on pursuing a goal is done here
      bool done = ExecuteCycle();
   
      // if we done, we'll disable run_flag and break this loop
      if (done) {
        env_->run_flag = false;
        env_->pause_flag = false;
        break;
      }
      // check if execution of the goal has completed in some way
   
      ros::WallDuration t_diff = ros::WallTime::now() - start;
      ROS_DEBUG_NAMED("service_robot", "Full control cycle time: %.9f\n", t_diff.toSec());
   
      r.sleep();
      // make sure to sleep for the remainder of our cycle time
      if (r.cycleTime() > ros::Duration(1 / co_->controller_frequency) && state_ == FIX_CONTROLLING) {
        GAUSSIAN_ERROR("Control loop missed its desired rate of %.4fHz... the loop actually took %.4f seconds", co_->controller_frequency, r.cycleTime().toSec());
      }
    } // while (n.ok())
  }
   
  // wake up the planner thread so that it can exit cleanly
  boost::unique_lock<boost::mutex> lock(planner_mutex_);
  runPlanner_ = true;
  planner_cond_.notify_one();
  lock.unlock();

  return true;
}

void AStarController::ClearFootprintInCostmap(const geometry_msgs::PoseStamped& pose, double clear_extend_dis, bool is_static_needed) {
  controller_costmap_ros_->clearFootprintInCostmap(pose.pose.position.x, pose.pose.position.y, 
                                                   tf::getYaw(pose.pose.orientation), clear_extend_dis);
  // clear current pose footprint on static costmap
  if (is_static_needed) {	
    controller_costmap_ros_->clearFootprintInCostmap(controller_costmap_ros_->getStaticCostmap(), pose.pose.position.x,
                                                     pose.pose.position.y, tf::getYaw(pose.pose.orientation), clear_extend_dis);
  }
}

bool AStarController::IsGoalUnreachable(const geometry_msgs::PoseStamped& goal_pose) {
    if (footprint_checker_->CircleCenterCost(goal_pose.pose.position.x,
                                             goal_pose.pose.position.y,
                                             tf::getYaw(goal_pose.pose.orientation),
                                             co_->circle_center_points, 0.0, 0.0) < -100.0) {
      return true;
    } else {
      return false;
    }
}

bool AStarController::IsGoalSafe(const geometry_msgs::PoseStamped& goal_pose, double goal_front_check_dis, double goal_back_check_dis, bool using_static_costmap) {
  footprint_checker_->setStaticCostmap(controller_costmap_ros_, using_static_costmap);

  if (!IsGoalFootprintSafe(0.5, 0.0, goal_pose)) {
    return false;
  }
  double resolution = controller_costmap_ros_->getCostmap()->getResolution();
  int front_num_step = goal_front_check_dis / resolution;
  int back_num_step = (-1) * goal_back_check_dis / resolution;
	
  double yaw = tf::getYaw(goal_pose.pose.orientation);
  std::vector<geometry_msgs::PoseStamped> path;
  for (int i = back_num_step; i <= front_num_step; ++i) {
    geometry_msgs::PoseStamped p;
    p.pose.position.x = goal_pose.pose.position.x + i * resolution * cos(yaw);
    p.pose.position.y = goal_pose.pose.position.y + i * resolution * sin(yaw);
    p.pose.orientation = goal_pose.pose.orientation;
    path.push_back(p);
  }
  for (int i = 0; i < path.size(); ++i) {
    if (footprint_checker_->CircleCenterCost(path[i].pose.position.x, path[i].pose.position.y, yaw, co_->circle_center_points, 0.0, 0.0) < 0) {
//    if (footprint_checker_->FootprintCost(path[i].pose.position.x, path[i].pose.position.y, yaw, footprint_spec_, 0.0, 0.0) < 0) {
      return false;
    }
  }
  return true;
}

bool AStarController::IsGoalFootprintSafe(double goal_safe_dis_a, double goal_safe_dis_b, const geometry_msgs::PoseStamped& pose) {
  std::vector<geometry_msgs::PoseStamped> fix_path = co_->fixpattern_path->GeometryPath();
  int goal_index = -1;
  for (int i = 0; i < static_cast<int>(fix_path.size()); ++i) {
    if (PoseStampedDistance(fix_path[i], pose) < 0.0001) {
      goal_index = i;
      break;
    }
  }
  if (goal_index == -1) {
    return true;
  }
  double free_dis_a = 0.0;
  for (int i = goal_index - 1; i >= 0; i -= 5) {
    double x = fix_path[i].pose.position.x;
    double y = fix_path[i].pose.position.y;
    double yaw = tf::getYaw(fix_path[i].pose.orientation);
    if (footprint_checker_->CircleCenterCost(x, y, yaw, co_->circle_center_points, 0.0, 0.0) < 0) {
//      GAUSSIAN_WARN("[ASTAR CONTROLLER] goal front not safe");
      return false;
    }
    free_dis_a += PoseStampedDistance(fix_path[i], fix_path[i + 5]);
    if (free_dis_a >= goal_safe_dis_a) {
      break;
    }
  }
  double free_dis_b = 0.0;
  for (int i = goal_index + 1; i < fix_path.size(); i += 5) {
    double x = fix_path[i].pose.position.x;
    double y = fix_path[i].pose.position.y;
    double yaw = tf::getYaw(fix_path[i].pose.orientation);
    if (footprint_checker_->CircleCenterCost(x, y, yaw, co_->circle_center_points, 0.0, 0.0) < 0) {
//      GAUSSIAN_WARN("[ASTAR CONTROLLER] goal back not safe");
      return false;
    }
    free_dis_b += PoseStampedDistance(fix_path[i], fix_path[i - 5]);
    if (free_dis_b >= goal_safe_dis_b) {
      break;
    }
  }
  return true;
}

bool AStarController::IsPathFootprintSafe(const std::vector<geometry_msgs::PoseStamped>& path,
                                          const std::vector<geometry_msgs::Point>& circle_center_points,
                                          double length) {
  double accu_dis = 0.0;
  for (int i = 0; i < path.size(); i += 5) {
    double yaw = tf::getYaw(path[i].pose.orientation);
    if (footprint_checker_->CircleCenterCost(path[i].pose.position.x, path[i].pose.position.y,
                                             yaw, circle_center_points, 0.0, 0.0) < 0) {
      return false;
    }
    if (i != 0) accu_dis +=PoseStampedDistance(path[i], path[i - 5]);
    if (accu_dis >= length) return true;
  }
  return true;
}

bool AStarController::IsPathFootprintSafe(const fixpattern_path::Path& fix_path, double length) {
  std::vector<geometry_msgs::PoseStamped> path = fix_path.GeometryPath();
  if (IsPathFootprintSafe(path, co_->circle_center_points, length)) {
    return true;
  }

  if (fabs(co_->sbpl_footprint_padding) < GS_DOUBLE_PRECISION) return false;
  GAUSSIAN_WARN("[ASTAR CONTROLLER] origin fix_path footprint is not safe");
	
  // if not safe, let's cast some padding to footprint
  std::vector<geometry_msgs::Point> circle_center_points_padding_1 = co_->circle_center_points;
  for (auto&& p : circle_center_points_padding_1) p.y +=co_->sbpl_footprint_padding;
  if (IsPathFootprintSafe(path, circle_center_points_padding_1, length)) {
    return true;
  }
  GAUSSIAN_WARN("[ASTAR CONTROLLER] pandding up fix_path footprint is not safe");

  // okay okay, the other padding
  std::vector<geometry_msgs::Point> circle_center_points_padding_2 = co_->circle_center_points;
  for (auto&& p : circle_center_points_padding_2) p.y -= co_->sbpl_footprint_padding;
  if (IsPathFootprintSafe(path, circle_center_points_padding_2, length)) {
    return true;
  }
  GAUSSIAN_WARN("[ASTAR CONTROLLER] pandding down fix_path footprint is not safe");

  // at last...
  return false;
}

double AStarController::CheckFixPathFrontSafe(const std::vector<geometry_msgs::PoseStamped>& path, double front_safe_check_dis, double extend_x, double extend_y, int begin_index) {
  double accu_dis = 0.0;
  double off_obstacle_dis = 0.0;
  bool cross_obstacle = false;
  int i, j;
  unsigned int temp_goal_index = 0;
  for (i = begin_index; i < path.size(); i += 5) {
    double yaw = tf::getYaw(path[i].pose.orientation);
    if (footprint_checker_->CircleCenterCost(path[i].pose.position.x, path[i].pose.position.y,
                                             yaw, co_->circle_center_points, extend_x, extend_y) < 0) {
      cross_obstacle = true;
      obstacle_index_ = i;
      break;
    }
    if (i != 0) accu_dis += PoseStampedDistance(path[i], path[i - 5]);
    if (temp_goal_index ==0 && accu_dis >= 1.5) temp_goal_index = i;
    if (accu_dis >= front_safe_check_dis) break;
  }
  if (!cross_obstacle && i >= path.size())
    accu_dis = front_safe_check_dis + 0.001;

  front_goal_index_ = temp_goal_index;
  return accu_dis;
}

bool AStarController::GetAStarStart(double front_safe_check_dis, double extend_x, double extend_y, int obstacle_index) {
  std::vector<geometry_msgs::PoseStamped> path = co_->fixpattern_path->GeometryPath();
  double accu_dis = 0.0;
  double off_obstacle_dis = 0.0;
  bool cross_obstacle = false;
  bool start_got = false;
  int i, j;
  if (obstacle_index >= path.size()) obstacle_index = 0;
  if (obstacle_index == 0) {
    for (i = 0; i < path.size(); i += 5) {
      double yaw = tf::getYaw(path[i].pose.orientation);
      if (footprint_checker_->CircleCenterCost(path[i].pose.position.x, path[i].pose.position.y,
                                               yaw, co_->circle_center_points, extend_x, extend_y) < 0) {
        cross_obstacle = true;
//        obstacle_index_ = i;
        obstacle_index = i;
        GAUSSIAN_INFO("[ASTAR CONTROLLER] GetAStarStart: obstacle_index = %d", obstacle_index);
        break;
      }
      if (i != 0) accu_dis += PoseStampedDistance(path[i], path[i - 5]);
      if (accu_dis >= front_safe_check_dis) break;
    }
  } else {
    cross_obstacle = true;
    accu_dis = 1.1;
  }
  if (cross_obstacle) {
    double start_dis;
    if (accu_dis > 1.2) {
      start_dis = 1.0;
    } else if (accu_dis > 1.0) {
      start_dis = 0.8;
    } else if (accu_dis > 0.7) {
      start_dis = 0.6;
    } else {
      start_dis = 0.0;
    }
    if (start_dis > 0.0) {
      for (j = obstacle_index; j > 2; j -= 2) {
        off_obstacle_dis += PoseStampedDistance(path[j], path[j - 2]);
        if (off_obstacle_dis > start_dis) {
          planner_start_ = path.at(j);
          start_got = true;
          GAUSSIAN_INFO("[ASTAR CONTROLLER] GetAStarStart: taken point front dis = %lf", accu_dis - off_obstacle_dis);
          break;
        }
      }
    } else {
      planner_start_ = path.front();
      GAUSSIAN_WARN("[ASTAR CONTROLLER] GetAStarStart: taken path.front as start point");
    }
/*
    if (accu_dis > 1.5) {
//      double start_dis = 1.3;
      double start_dis = accu_dis > 1.7 ?  accu_dis - 0.55 : 1.15;
      for (j = obstacle_index_; j > 2; j -= 2) {
        off_obstacle_dis += PoseStampedDistance(path[j], path[j - 2]);
        if (off_obstacle_dis > start_dis) {
          planner_start_ = path.at(j);
          start_got = true;
          GAUSSIAN_INFO("[ASTAR CONTROLLER] GetAStarStart: taken point front dis = %lf", accu_dis - off_obstacle_dis);
          break;
        }
      }
   } else if (accu_dis > 1.0) {
      double start_dis = accu_dis > 1.3 ?  accu_dis - 0.35 : 0.95;
      for (j = obstacle_index_; j > 2; j -= 2) {
        off_obstacle_dis += PoseStampedDistance(path[j], path[j - 2]);
        if (off_obstacle_dis > start_dis) {
          planner_start_ = path.at(j);
          start_got = true;
          GAUSSIAN_INFO("[ASTAR CONTROLLER] GetAStarStart: taken point front dis = %lf", accu_dis - off_obstacle_dis);
          break;
        }
      }
    } else if (accu_dis > 0.7) {
      double start_dis = accu_dis > 0.85 ?  accu_dis - 0.15 : 0.70;
      for (j = obstacle_index_; j > 2; j -= 2) {
        off_obstacle_dis += PoseStampedDistance(path[j], path[j - 2]);
        if (off_obstacle_dis > start_dis) {
          planner_start_ = path.at(j);
          start_got = true;
          GAUSSIAN_INFO("[ASTAR CONTROLLER] GetAStarStart: taken point front dis = %lf", accu_dis - off_obstacle_dis);
          break;
        }
      }
    }else {
      if (path.size() > 20) {
        planner_start_ = path.at(10);
        GAUSSIAN_WARN("[ASTAR CONTROLLER] GetAStarStart: taken point front index = 10");
      } else {
        planner_start_ = path.front();
        GAUSSIAN_WARN("[ASTAR CONTROLLER] GetAStarStart: taken path.front as start point");
      }
    }
*/
  }
  planner_start_.header.frame_id = co_->global_frame;
  astar_start_pub_.publish(planner_start_);
  return start_got;
}

bool AStarController::IsFixPathFrontSafe(double front_safe_check_dis) {

  std::vector<geometry_msgs::PoseStamped> path = co_->fixpattern_path->GeometryPath();
  if (IsPathFootprintSafe(path, co_->circle_center_points, front_safe_check_dis)) {
    return true;
  }

  GAUSSIAN_WARN("[Fixpattern_path] origin path is not safe");
  if (fabs(co_->fixpattern_footprint_padding) < GS_DOUBLE_PRECISION) return false;

  // if not safe, let's cast some padding to footprint
  std::vector<geometry_msgs::Point> circle_center_points_padding_1 = co_->circle_center_points;
  for (auto&& p : circle_center_points_padding_1) p.y +=co_->fixpattern_footprint_padding;
  if (IsPathFootprintSafe(path, circle_center_points_padding_1, front_safe_check_dis)) {
    return true;
  }
  GAUSSIAN_WARN("[Fixpattern_path] Pandding up path is not safe");

  // okay okay, the other padding
  std::vector<geometry_msgs::Point> circle_center_points_padding_2 = co_->circle_center_points;
  for (auto&& p : circle_center_points_padding_2) p.y -= co_->fixpattern_footprint_padding;
  if (IsPathFootprintSafe(path, circle_center_points_padding_2, front_safe_check_dis)) {
    return true;
  }
  GAUSSIAN_WARN("[Fixpattern_path] Pandding down path is not safe");

  // at last...
  return false;
}

bool AStarController::NeedBackward(const geometry_msgs::PoseStamped& pose, double distance) {
  double yaw = tf::getYaw(pose.pose.orientation);
  double resolution = controller_costmap_ros_->getCostmap()->getResolution() / 3.0;
  int num_step = distance / resolution;
//  GAUSSIAN_INFO("[ASTAR CONTROLLER] needbackward check: distance = %lf, num_step = %d", distance, num_step);
	
  std::vector<geometry_msgs::PoseStamped> path;
  for (int i = 0; i <= num_step; ++i) {
    geometry_msgs::PoseStamped p;
    p.pose.position.x = pose.pose.position.x + i * resolution * cos(yaw);
    p.pose.position.y = pose.pose.position.y + i * resolution * sin(yaw);
    p.pose.orientation = pose.pose.orientation;
    path.push_back(p);
  }
  for (int i = 0; i < path.size(); ++i) {
    if (footprint_checker_->CircleCenterCost(path[i].pose.position.x, path[i].pose.position.y, yaw,
                                             co_->footprint_center_points, 0.0, 0.0) < 0) {
      GAUSSIAN_INFO("[ASTAR CONTROLLER] distance = %lf, not safe step = %d", distance, i);
      return true;
    }
  }
  return false;
}

double AStarController::PoseStampedDistance(const geometry_msgs::PoseStamped& p1, const geometry_msgs::PoseStamped& p2) {
  return hypot(p1.pose.position.x - p2.pose.position.x, p1.pose.position.y - p2.pose.position.y);
}

bool AStarController::ExecuteCycle() {
  // we need to be able to publish velocity commands
  double t0, t1, t2, t3, t4, t5;
  t0 = GetTimeInSeconds();

  geometry_msgs::Twist cmd_vel;
  // get curent position
  tf::Stamped<tf::Pose> global_pose;
  geometry_msgs::PoseStamped current_position;
  if (!controller_costmap_ros_->getRobotPose(global_pose)) {
    GAUSSIAN_ERROR("[ASTAR CONTROLLER] cannot get current position, terminate this ExecuteCycle");
    return false;
  } else {
    tf::poseStampedTFToMsg(global_pose, current_position);
  }
  double cur_goal_distance = PoseStampedDistance(current_position, global_goal_);
//  GAUSSIAN_INFO("[ASTAR CONTROLLER]:cur_goal_distance = %lf", cur_goal_distance);
  // check to see if we've moved far enough to reset our oscillation timeout
  if (PoseStampedDistance(current_position, oscillation_pose_) >= co_->oscillation_distance) {
    last_oscillation_reset_ = ros::Time::now();
    oscillation_pose_ = current_position;
  }

  // check that the observation buffers for the costmap are current, we don't want to drive blind
  controller_costmap_ros_->getCostmap();
  if (!controller_costmap_ros_->isCurrent()) {
    GAUSSIAN_WARN("[%s]:Sensor data is out of date, we're not going to allow commanding of the base for safety", ros::this_node::getName().c_str());
    PublishZeroVelocity();
    // TODO(chenkan) do something to notify GUI
    return false;
  }

  // if we have a new plan then grab it and give it to the controller
  // TODO(chenkan): need to check if planner_mutex_ needs to be locked
  // for new_global_plan_ here
  if (new_global_plan_) {
    // make sure to set the new plan flag to false
    new_global_plan_ = false;
    GAUSSIAN_INFO("[ASTAR CONTROLLER] get new plan");
    ROS_DEBUG_NAMED("service_robot", "Got a new plan...swap pointers");
  
    // in case new plan has different rotate dir
    co_->fixpattern_local_planner->reset_planner();

    if (gotInitPlan_) {
      footprint_checker_->setStaticCostmap(controller_costmap_ros_, false);
    }
    ROS_DEBUG_NAMED("service_robot", "pointers swapped!");
  }
  if (!localization_valid_) {
    state_ = FIX_CLEARING;
    recovery_trigger_ = LOCATION_RECOVERY_R;
  }
  
  t1 = GetTimeInSeconds();
  if (t1 - t0 > 0.02) {
    GAUSSIAN_INFO("get costmap cost %lf sec", t1 - t0);
  }
  // the move_base state machine, handles the control logic for navigation
  switch (state_) {
    // if we are in a planning state, then we'll attempt to make a plan
    case A_PLANNING:
      GAUSSIAN_INFO("[ASTAR CONTROLLER] in PLANNING state");
      {
        boost::mutex::scoped_lock lock(planner_mutex_);
        runPlanner_ = true;
        planner_cond_.notify_one();
      }
      ROS_DEBUG_NAMED("service_robot", "Waiting for plan, in the planning state.");
      break;

    case FIX_CONTROLLING:
      GAUSSIAN_INFO("[FIXPATTERN CONTROLLER] in FIX_CONTROLLING state");
      ROS_DEBUG_NAMED("service_robot", "In controlling state.");

      // check to see if we've reached our goal
      if (co_->fixpattern_local_planner->isGoalReached()) {
        GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] fixpattern goal reached");
        ROS_DEBUG_NAMED("service_robot", "Goal reached!");
        PublishZeroVelocity();
        ResetState();
        // reset fixpattern_local_planner
        co_->fixpattern_local_planner->reset_planner();	

        // we need to notify fixpattern_path
        co_->fixpattern_path->FinishPath();

        // check is global goal reached
        if (!IsGlobalGoalReached(current_position, global_goal_, 
                                 co_->fixpattern_local_planner->xy_goal_tolerance_, co_->fixpattern_local_planner->yaw_goal_tolerance_)) {
          PublishZeroVelocity();
          state_ = FIX_CLEARING;
          recovery_trigger_ = FIX_GETNEWGOAL_R;
          GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] global goal not reached yet, swtich to CLEARING state and get new goal");
          break;  //(lee)
        } else {
          // publish goal reached 
          if (global_goal_type_ == CHARGING) {
            HeadingChargingGoal(charging_goal_);
            co_->fixpattern_local_planner->resetGoalTolerance();
          }
          PublishGoalReached(global_goal_);
          PublishMovebaseStatus(I_GOAL_REACHED);
          GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] global goal reached, teminate controller");
          return true;  //(lee)
        }
      }
/*
      {
        // check if need going back
        if (HandleGoingBack(current_position) && fabs(last_valid_cmd_vel_.linear.x) > 0.08) {
          PublishZeroVelocity();
          state_ = FIX_CLEARING;
          recovery_trigger_ = FIX_GETNEWGOAL_R;
          GAUSSIAN_ERROR("[FIXPATTERN CONTROLLER] HandleGoingBack, entering A_PLANNING state");
          return false;
        }
      }
*/
      // check if swtich to origin path needed
			// TODO(lizhen) check bug here
      HandleSwitchingPath(current_position);

      t2 = GetTimeInSeconds();
      if (t2 - t1 > 0.04) {
        GAUSSIAN_INFO("check reached goal and HandleSwitch cost %lf sec", t2 - t1);
      }
      // we'll Prune the path first as we don't want to navigate back when trigger front_safe while robot still moves
      // we'll not prune any point when first run 
      if(first_run_controller_flag_) {
        first_run_controller_flag_ = false;
      } else if (!co_->fixpattern_local_planner->isGoalXYLatched()) {
        if(co_->fixpattern_local_planner->isRotatingToGoalDone()) {
          co_->fixpattern_path->PruneCornerOnStart();
          co_->fixpattern_local_planner->resetRotatingToGoalDone();
          GAUSSIAN_INFO("[FIXPATTERN CONTROLLER] Prune Corner Point On Start");  
        } else {
          // get current pose of the vehicle && prune fixpattern path
          if (!co_->fixpattern_path->Prune(fixpattern_path::GeometryPoseToPathPoint(current_position.pose), co_->max_offroad_dis, co_->max_offroad_yaw, true)) {
            GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] Prune fix path failed, swtich to FIX_CLEARING");  
            PublishZeroVelocity();
            state_ = FIX_CLEARING;
            recovery_trigger_ = FIX_GETNEWGOAL_R;
            break;
          }
        }
      }
/*
      if (!runPlanner_ &&
          PoseStampedDistance(current_position, sbpl_planner_goal_) <= 0.5 &&
          PoseStampedDistance(sbpl_planner_goal_, global_goal_) >= 0.1) {
        // just restart the planner, and we'll not stop during this time
        boost::unique_lock<boost::mutex> lock(planner_mutex_);
        planning_state_ = P_INSERTING_SBPL;
        runPlanner_ = true;
        planner_cond_.notify_one();
        lock.unlock();
        GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] distance to sbpl goal < 0.5, replan");
      }
*/
      t3 = GetTimeInSeconds();
      if (t3 - t2 > 0.04) {
        GAUSSIAN_INFO("Prune path cost %lf sec", t3 - t2);
      }

      // check for an oscillation condition
      if (co_->oscillation_timeout > 0.0 &&
         last_oscillation_reset_ + ros::Duration(co_->oscillation_timeout) < ros::Time::now()) {
        GAUSSIAN_INFO("[FIXPATTERN CONTROLLER] oscillation to CLEARING");
        PublishZeroVelocity();
        state_ = FIX_CLEARING;
        recovery_trigger_ = FIX_OSCILLATION_R;
      }

			// check for protector status and handle going back if front detected
      if (CheckProtector(current_position)) {
        GAUSSIAN_ERROR("[FIXPATTERN CONTROLLER] check front protector, then swtich to FIX_GETNEWGOAL_R state");
        state_ = FIX_CLEARING;
        recovery_trigger_ = FIX_GETNEWGOAL_R;
        break;
      }
 
			// check for front path or goal is safe or not  
      {      
        cmd_vel_ratio_ = 1.0;
        std::vector<geometry_msgs::PoseStamped> fix_path = co_->fixpattern_path->GeometryPath();
        double front_safe_dis = CheckFixPathFrontSafe(fix_path, co_->front_safe_check_dis, 0.0, 0.0);
        // when cur pose is closed to global_goal, check if goal safe 
        if (cur_goal_distance < co_->goal_safe_check_dis
            && front_safe_dis < co_->front_safe_check_dis
            && !IsGoalSafe(global_goal_, 0.10, 0.15)) { 
          if (front_safe_dis < 0.35) {
            PublishVelWithAcc(last_valid_cmd_vel_, co_->stop_to_zero_acc);
//            PublishZeroVelocity();
            PublishMovebaseStatus(E_GOAL_NOT_SAFE);
            bool is_goal_safe = false;
            ros::Rate check_rate(10);
            ros::Time check_end_time = ros::Time::now() + ros::Duration(co_->goal_safe_check_duration);
            unsigned int check_goal_safe_cnt = 0;
            while (ros::Time::now() < check_end_time && env_->run_flag) {
              if (IsGoalSafe(global_goal_, 0.10, 0.15)) {
                if (++check_goal_safe_cnt > 5) {
                  is_goal_safe = true;
                  GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] Check global goal safe, continue!");
                  break;
                }
              } else {
                check_goal_safe_cnt = 0;
                PublishMovebaseStatus(E_GOAL_NOT_SAFE);
              }
              GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] Check global goal not safe, stop here!");
              check_rate.sleep();
            }
            if (!is_goal_safe){      
              // publish goal unreached
              if (env_->run_flag) {
                PublishGoalReached(current_position);
              }
              PublishMovebaseStatus(I_GOAL_UNREACHED);

              GAUSSIAN_ERROR("[FIXPATTERN CONTROLLER] Check global goal not safe, terminate!");
              // disable the planner thread
              boost::unique_lock<boost::mutex> lock(planner_mutex_);
              runPlanner_ = false;
              lock.unlock();

              ResetState();
              // we need to notify fixpattern_path
              co_->fixpattern_path->FinishPath();

              // TODO(chenkan): check if this is needed
              co_->fixpattern_local_planner->reset_planner();
              // Goal not reached, but we will stop and exit 
              return true;
            }
          }
        } else if (front_safe_dis < co_->front_safe_check_dis) { // check front safe distance
          if (front_safe_dis <= 0.6) {
            front_safe_check_cnt_ = 0;
            if (front_safe_dis <= 0.2) {
              PublishZeroVelocity();
            } else {
              PublishVelWithAcc(last_valid_cmd_vel_, co_->stop_to_zero_acc);
            }

            ros::Time end_time = ros::Time::now() + ros::Duration(co_->stop_duration);
            ros::Time start_plan_time = ros::Time::now() + ros::Duration(co_->stop_duration - 0.7);
            ros::Rate r(10);
            bool front_safe = false;
            unsigned int front_safe_cnt = 0;
            unsigned int waiting_cnt = 0;
            switch_path_ = false;
            controller_costmap_ros_->getRobotPose(global_pose);
            tf::poseStampedTFToMsg(global_pose, current_position);
            while (ros::Time::now() < end_time && env_->run_flag) {
              front_safe_dis = CheckFixPathFrontSafe(fix_path, co_->front_safe_check_dis, 0.0, 0.0);
              PublishMovebaseStatus(E_PATH_NOT_SAFE);
              if (front_safe_dis > 1.0) {
                if (++front_safe_cnt > 2) {
                  front_safe = true;
                  break;
                }
              } else if (++waiting_cnt > 3 && ros::Time::now() > start_plan_time && !runPlanner_ && !switch_path_){
                GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] front not safe, stop here and enable PlanThread");
                if (GetAStarGoal(current_position, 0.0, 0.0, obstacle_index_)) {
                  planning_state_ = P_INSERTING_BEGIN;
                  // enable the planner thread in case it isn't running on a clock
                  boost::unique_lock<boost::mutex> lock(planner_mutex_);
                  runPlanner_ = true;
                  planner_cond_.notify_one();
                  lock.unlock();
                }
              }
              GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] path front not safe, dis = %lf <= 0.6, stop here until stop_duration", front_safe_dis);
              r.sleep();
            }
            if(!front_safe) {
              PublishZeroVelocity();
              controller_costmap_ros_->getRobotPose(global_pose);
              tf::poseStampedTFToMsg(global_pose, current_position);
              if (HandleGoingBack(current_position) || !switch_path_ || 
                  (switch_path_ && PoseStampedDistance(current_position, co_->fixpattern_path->GeometryPath().front()) > 0.07)) {
                GAUSSIAN_ERROR("[FIXPATTERN CONTROLLER] !IsPathFrontSafe dis = %lf, stop and switch to CLEARING", front_safe_dis);
                state_ = FIX_CLEARING;
                recovery_trigger_ = FIX_GETNEWGOAL_R;
              } else if (switch_path_) {
                GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] path front not safe, using pre planning path and continue");
              }
            } else if (switch_path_) {
              // clear local planner error cnt, to avoid it stop again
              fix_local_planner_error_cnt_ = 0;
              HandleSwitchingPath(current_position, true);
              GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] pre path front change safe again, switch to pre path");
            }
            break;
          } else {
            GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] !IsPathFrontSafe dis = %lf > 0.5, check_cnt = %d", front_safe_dis, front_safe_check_cnt_);
            if (front_safe_dis < 1.0) {
              cmd_vel_ratio_ = 0.5;
            } else if (front_safe_dis < 1.7) {
              cmd_vel_ratio_ = 0.7;
            }
            if (!runPlanner_ && ++front_safe_check_cnt_ > 10) {
              if (front_safe_dis < 0.6) {
                GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] path front not safe, dis = %lf <= 0.6, try to stop here", front_safe_dis);
                if (front_safe_dis <= 0.3) {
                  PublishZeroVelocity();
                } else {
                  PublishVelWithAcc(last_valid_cmd_vel_, co_->stop_to_zero_acc);
                }
                break;
              } else if (front_safe_dis < 1.5) {
                GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] Enable PlanThread and continue FIX_CONTROLLING");
//                CheckFixPathFrontSafe(fix_path, co_->front_safe_check_dis, true, 0.0, 0.0); // get planner_start
                if (GetAStarGoal(current_position, 0.0, 0.0, obstacle_index_)) {
                  planning_state_ = P_INSERTING_MIDDLE;
                  // enable the planner thread in case it isn't running on a clock
                  boost::unique_lock<boost::mutex> lock(planner_mutex_);
                  runPlanner_ = true;
                  planner_cond_.notify_one();
                  lock.unlock();
                }
              } else {
                --front_safe_check_cnt_;
              }
            }
          }
        } else {
          front_safe_check_cnt_ = 0;
        }
      }

      t4 = GetTimeInSeconds();
      if (t4 - t3 > 0.04) {
        GAUSSIAN_INFO("Check front path cost %lf sec", t4 - t3);
      }

      {
        if (!co_->fixpattern_local_planner->setPlan(co_->fixpattern_path->path(), co_->global_frame)) {
          // ABORT and SHUTDOWN COSTMAPS
          GAUSSIAN_ERROR("Failed to pass global plan to the controller, aborting.");
          ResetState();
          return true;
        }
        std::vector<geometry_msgs::PoseStamped> plan = co_->fixpattern_path->GeometryPath();
        for (auto&& p : plan) {  // NOLINT
          p.header.frame_id = co_->global_frame;
          p.header.stamp = ros::Time::now();
        }
        PublishPlan(fixpattern_pub_, plan);
      }

      {
        // get cmd_vel 
        bool local_planner_ret = co_->fixpattern_local_planner->computeVelocityCommands(fixpattern_local_planner::TRAJECTORY_PLANNER, &cmd_vel);    
        if (!local_planner_ret) {
          ++fix_local_planner_error_cnt_;
          cmd_vel = last_valid_cmd_vel_;
          GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] local_planner error count = %d", fix_local_planner_error_cnt_);
          // check if need going back
          if (cmd_vel.linear.x > 0.10 && NeedBackward(current_position, 0.05)) {
            GAUSSIAN_ERROR("[FIXPATTERN CONTROLLER] !IsFrontSafe ,stop and switch to CLEARING");
            PublishZeroVelocity();
            state_ = FIX_CLEARING;
            // TODO(lizhen): to set FIX_GETNEWGOAL_R or BACKWARD_RECOVERY_R
            recovery_trigger_ = FIX_GETNEWGOAL_R;
          }
        } else {
          fix_local_planner_error_cnt_ = 0;
          local_planner_timeout_cnt_ = 0;
          last_valid_cmd_vel_ = cmd_vel;
        }

        if (fix_local_planner_error_cnt_ < 3) {
          ROS_DEBUG_NAMED("service_robot", "Got a valid command from the local planner: %.3lf, %.3lf, %.3lf",
                          cmd_vel.linear.x, cmd_vel.linear.y, cmd_vel.angular.z);
          last_valid_control_ = ros::Time::now();
          cmd_vel.linear.x *= cmd_vel_ratio_;	
          cmd_vel.angular.z *= cmd_vel_ratio_;	
          if (fix_local_planner_error_cnt_ > 0) {
            cmd_vel.linear.x *= 0.75;
            cmd_vel.angular.z *= 0.75;	
          }
          if (fabs(cmd_vel.angular.z) < 0.18 && fabs(cmd_vel.angular.z) > 0.08) {
            cmd_vel.angular.z = cmd_vel.angular.z > 0.0 ? 0.18 : -0.18; 
          }
          // make sure that we send the velocity command to the base
          co_->vel_pub->publish(cmd_vel);
          last_valid_cmd_vel_ = cmd_vel;
          // notify room_server to play_sound 
          PublishHeadingGoal();
          // notify gs_consle
          PublishMovebaseStatus(I_GOAL_HEADING);
        } else {
          ROS_DEBUG_NAMED("service_robot", "The local planner could not find a valid plan.");
          ros::Time attempt_end = last_valid_control_ + ros::Duration(co_->controller_patience);

          // check if we've tried to find a valid control for longer than our time limit
          if (ros::Time::now() > attempt_end) {
            // we'll move into our obstacle clearing mode
            // TODO(lizhen): check this variable
//            fix_local_planner_error_cnt_ = 0;
            ++local_planner_timeout_cnt_;
            PublishZeroVelocity();
            state_ = FIX_CLEARING;
            recovery_trigger_ = BACKWARD_RECOVERY_R;
            GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] CONTROLLING exceeds attempt_end, local_planner_timeout_cnt = %d", local_planner_timeout_cnt_);
/*
            if (!co_->fixpattern_local_planner->isFootprintSafe()) {
              recovery_trigger_ = BACKWARD_RECOVERY_R;
            } else {
              recovery_trigger_ = LOCAL_PLANNER_RECOVERY_R;
            }
*/
            break;
          } else {
            // otherwise, if we can't find a valid control, we'll retry, until
            GAUSSIAN_INFO("[FIXPATTERN CONTROLLER] wait for a valid control");
            // reach controller_patience
            state_ = FIX_CONTROLLING;
            PublishZeroVelocity();
            break;
          }
        }
      }

      t5 = GetTimeInSeconds();
      if (t5 - t4 > 0.06) {
        GAUSSIAN_INFO("Local planner cost %lf sec", t5 - t4);
      }

      break;
			
    // we'll try to launch recovery behaviors
    case FIX_CLEARING:
      GAUSSIAN_INFO("[FIX CONTROLLER] in FIX_CLEARING state");
      if (recovery_trigger_ == LOCATION_RECOVERY_R) {
        GAUSSIAN_WARN("[FIX CONTROLLER] in LOCATION_RECOVERY_R state");
        ros::Time end_time = ros::Time::now() + ros::Duration(co_->localization_duration);
        ros::Rate r(10);
        while (ros::Time::now() < end_time && !localization_valid_) {
          GAUSSIAN_WARN("[FIX CONTROLLER] CLEARING state: waiting for valid localization");
          r.sleep();
        }
        if (LocalizationRecovery()) {
          PublishZeroVelocity();
          state_ = FIX_CLEARING;
          recovery_trigger_ = FIX_GETNEWGOAL_R;
        }
        break;
      }

      if (recovery_trigger_ == BACKWARD_RECOVERY_R) {
        GAUSSIAN_WARN("[FIX CONTROLLER] in BACKWARD_RECOVERY_R state");
        PublishMovebaseStatus(E_PATH_NOT_SAFE);
        if (HandleGoingBack(current_position, co_->backward_check_dis)) {
//        GoingBackward(0.15);
          controller_costmap_ros_->getRobotPose(global_pose);
          tf::poseStampedTFToMsg(global_pose, current_position);
          PublishZeroVelocity();
          state_ = FIX_CLEARING;
          recovery_trigger_ = FIX_GETNEWGOAL_R;
        } else {
          recovery_trigger_ = LOCAL_PLANNER_RECOVERY_R;
        }
      }

      if (recovery_trigger_ == LOCAL_PLANNER_RECOVERY_R) {
        PublishMovebaseStatus(E_PATH_NOT_SAFE);
        if (local_planner_timeout_cnt_ > 10) {
          PublishMovebaseStatus(I_GOAL_UNREACHABLE);
          env_->run_flag = false;
          env_->pause_flag = false;
          GAUSSIAN_ERROR("[FIXPATTERN CONTROLLER] LOCAL_PLANNER_RECOVERY_R: local_planner_timeout_cnt_ > 10, set run_flag false and return here!");
          break;
				} else if (local_planner_timeout_cnt_ > 5 || fix_local_planner_error_cnt_ > 7) {
          GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] LOCAL_PLANNER_RECOVERY_R: local_planner_timeout_cnt_ = %d > 5!, clear footprint on costmap, and switch to GLOBAL_PLANNER_RECOVERY_R", local_planner_timeout_cnt_);
          ClearFootprintInCostmap(current_position, 0.05, false);
          state_ = FIX_CONTROLLING;
          break;
        }
        recovery_trigger_ = GLOBAL_PLANNER_RECOVERY_R;
      }

      if (recovery_trigger_ == GLOBAL_PLANNER_RECOVERY_R) {
        // we will try Going Back first
        HandleGoingBack(current_position, co_->backward_check_dis + 0.05);
        // check if oboscal in footprint, yes - recovery; no - get new goal and replan
        controller_costmap_ros_->getRobotPose(global_pose);
        tf::poseStampedTFToMsg(global_pose, current_position);
        if (footprint_checker_->FootprintCost(current_position, unpadded_footrpint_spec_, 0.0, 0.0) < 0.0 ||
            footprint_checker_->BroaderFootprintCost(current_position, footprint_spec_, co_->recovery_footprint_extend_x, co_->recovery_footprint_extend_y) < 0.0) {
        // if (footprint_checker_->BroaderFootprintCost(current_position, footprint_spec_, co_->recovery_footprint_extend_x, co_->recovery_footprint_extend_y) < 0.0) {
          GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] GLOBAL_PLANNER_RECOVERY_R: footprint cost check < 0!, switch to Recovery");
          PublishMovebaseStatus(E_PATH_NOT_SAFE);
          EscapeRecovery(current_position);
          state_ = FIX_CLEARING;
          recovery_trigger_ = FIX_GETNEWGOAL_R;
          break;
        } else {
          GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] GLOBAL_PLANNER_RECOVERY_R: footprint cost check OK! cheking astar_planner_timeout_cnt_ = %d, try_recovery_times_ = %d", astar_planner_timeout_cnt_, try_recovery_times_);
          if ((astar_planner_timeout_cnt_ > 12 || try_recovery_times_ > 8) && !co_->use_farther_planner) {
            PublishMovebaseStatus(I_GOAL_UNREACHABLE);
            env_->run_flag = false;
            env_->pause_flag = false;
            GAUSSIAN_ERROR("[FIXPATTERN CONTROLLER] GLOBAL_PLANNER_RECOVERY_R: astar_planner_timeout_cnt_ > 12 || try_recovery_times_ > 6, set run_flag false and return here!");
            break;
          }
          if (try_recovery_times_ > 7) {
            GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] GLOBAL_PLANNER_RECOVERY_R: try_recovery_times_ > 7, ClearFootprintInCostmap");
            ClearFootprintInCostmap(current_position, 0.05, false);
          }
          if (astar_planner_timeout_cnt_ > 2) {
            GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] GLOBAL_PLANNER_RECOVERY_R: Handle Rotate Recovery");
            RotateRecovery();
          }
          state_ = FIX_CLEARING;
          recovery_trigger_ = FIX_GETNEWGOAL_R;
        }
      }

      // we'll invoke recovery behavior
      if (recovery_trigger_ == FIX_GETNEWGOAL_R) {
        GAUSSIAN_INFO("[FIX CONTROLLER] in CLEARING state: FIX_GETNEWGOAL_R");
        PublishZeroVelocity();

        bool new_goal_got = false; 
        if (try_recovery_times_ > 6 && co_->use_farther_planner && IsGoalSafe(global_goal_, 0.10, 0.15)) {
          GAUSSIAN_WARN("[FIX CONTROLLER] CLEARING state: try_recovery_times_> 5, got global goal as astar_goal_!");
          try_recovery_times_ = 0; 
          new_goal_got = true;
          planner_goal_ = global_goal_;
          taken_global_goal_ = true;
        } else if (astar_planner_timeout_cnt_ > 5 && GetAStarTempGoal(planner_goal_, 1.0)) {
//          astar_planner_timeout_cnt_ = 0;
          new_goal_got = true;
          GAUSSIAN_WARN("[FIX CONTROLLER] CLEARING state: astar_planner_timeout_cnt_ > 5, got temp AStar Goal success! Switch to A_PLANNING");
        } else {
          // get a new astar goal
          ros::Time end_time = ros::Time::now() + ros::Duration(co_->stop_duration / 2.0);
          ros::Rate r(10);
          while (ros::Time::now() < end_time) {
            if (GetAStarGoal(current_position, 0.0, 0.0)) {
              new_goal_got = true;
              break;
            }
            last_valid_control_ = ros::Time::now();
            r.sleep();
          }
          // if get astar goal failed, try to get a temp goal
          if (!new_goal_got && GetAStarTempGoal(planner_goal_, 1.0)) {
            new_goal_got = true;
            GAUSSIAN_INFO("[FIX CONTROLLER] CLEARING state: got temp AStar Goal success! Switch to A_PLANNING");
          }
        }
				
        // find a new safe goal, use it to replan
        if (new_goal_got) {
          state_ = A_PLANNING;
          recovery_trigger_ = A_PLANNING_R;
          boost::unique_lock<boost::mutex> lock(planner_mutex_);
          if (taken_global_goal_) { 
            planning_state_ = P_INSERTING_NONE;
          } else {
            planning_state_ = P_INSERTING_BEGIN;
          }
          lock.unlock();
          GAUSSIAN_INFO("[FIX CONTROLLER] CLEARING state: got AStar Goal success! Switch to A_PLANNING");
        } else {
          // TODO(lizhen) Alarm here, and try to get AStar goal again 
          state_ = FIX_CLEARING;
          recovery_trigger_ = FIX_GETNEWGOAL_R;
          GAUSSIAN_ERROR("[FIX CONTROLLER] CLEARING state: got AStar Goal failed! Alarm and try again");
/*
          // no point is safe, so terminate the path
          ResetState();
          // disable the planner thread
          boost::unique_lock<boost::mutex> lock(planner_mutex_);
          runPlanner_ = false;
          lock.unlock();

          // TODO(chenkan): check if this is needed
          co_->fixpattern_local_planner->reset_planner();

          // we need to notify fixpattern_path
          co_->fixpattern_path->FinishPath();

          GAUSSIAN_ERROR("[FIX CONTROLLER] GetAStarGoal failed, terminate path");
          // TODO(lizhen) Alarm here
          return true;
*/
        }
      }

      break;

    default:
      GAUSSIAN_ERROR("This case should never be reached, something is wrong, aborting");
      ResetState();
      // disable the planner thread
      boost::unique_lock<boost::mutex> lock(planner_mutex_);
      runPlanner_ = false;
      lock.unlock();
      // Reached a case that should not be hit in service_robot. This is a bug, please report it.
      return true;
  }

  // we aren't done yet
  return false;
}

void AStarController::ResetState() {
  // Disable the planner thread
  boost::unique_lock<boost::mutex> lock(planner_mutex_);
  runPlanner_ = false;
  lock.unlock();

  // Reset statemachine
  state_ = A_PLANNING;
  recovery_trigger_ = A_PLANNING_R;
  PublishZeroVelocity();
  front_path_.FinishPath();
  switch_path_ = false;
  origin_path_safe_cnt_ = 0;
/*
  state_ = F_CONTROLLING;
  recovery_trigger_ = F_CONTROLLING_R;
  PublishZeroVelocity();
*/
//  GAUSSIAN_INFO("[ASTAR CONTROLLER] ResetState");
  // search planner goal from start
  planner_goal_index_ = 0;
  cmd_vel_ratio_ = 1.0;
  astar_planner_timeout_cnt_ = 0;
  try_recovery_times_ = 0;
  obstacle_index_ = 0;
  // reset some variables
  using_sbpl_directly_ = false;
  last_using_bezier_ = false;
  replan_directly_ = false;
  localization_valid_ = false;
  first_run_controller_flag_ = true;
  gotInitPlan_ = false;
}

bool AStarController::IsGlobalGoalReached(const geometry_msgs::PoseStamped& current_position, const geometry_msgs::PoseStamped& global_goal,
                                            double xy_goal_tolerance, double yaw_goal_tolerance) {
  double pose_diff = PoseStampedDistance(current_position, global_goal);
  double yaw_diff = angles::shortest_angular_distance(tf::getYaw(current_position.pose.orientation), tf::getYaw(global_goal.pose.orientation));
//  if ((co_->fixpattern_local_planner->isGoalXYLatched() || pose_diff < xy_goal_tolerance) && fabs(yaw_diff) < yaw_goal_tolerance) {
  GAUSSIAN_WARN("IsGlobalGoalReached: pose_diff = %lf, yaw_diff = %lf", pose_diff, yaw_diff);
  if (pose_diff > 1.0 || fabs(yaw_diff) > M_PI / 3.0) {
    return false;
  } else {
    return true;
  }
}

bool AStarController::GetCurrentPosition(geometry_msgs::PoseStamped& current_position) {
  tf::Stamped<tf::Pose> global_pose;
  geometry_msgs::PoseStamped cur_pos;
  if (!controller_costmap_ros_->getRobotPose(global_pose)) {
    GAUSSIAN_WARN("Unable to get current_position");
    return false;
  } else {
    tf::poseStampedTFToMsg(global_pose, cur_pos);
    current_position = cur_pos;
    return true;
  }
}

unsigned int AStarController::GetPoseIndexOfPath(const std::vector<geometry_msgs::PoseStamped>& path, const geometry_msgs::PoseStamped& pose) {
  unsigned int index = 0x7FFFFFFF;
  for (unsigned int i = 0; i < static_cast<int>(path.size()); ++i) {
    if (PoseStampedDistance(path.at(i), pose) < 0.001) {
      index = i;
      break;
    }
  }
  return index;
}

bool AStarController::GetAStarGoal(const geometry_msgs::PoseStamped& cur_pose, double extend_x, double extend_y, int begin_index) {
  double start = GetTimeInSeconds();
  double cur_goal_dis = PoseStampedDistance(cur_pose, global_goal_);
 /* if (path.size() == 0) {
	  GAUSSIAN_WARN("[ASTAR CONTROLLER] GetAStarGoal failed, path_size = %zu == 0", path.size());
    return false; 
  }
*/
//  if (planning_state_ == P_INSERTING_BEGIN) {
  if (true) {
    co_->fixpattern_path->Prune(fixpattern_path::GeometryPoseToPathPoint(cur_pose.pose), co_->max_offroad_dis, co_->max_offroad_yaw, true);
  }
  std::vector<geometry_msgs::PoseStamped> path = co_->fixpattern_path->GeometryPath();
  GAUSSIAN_INFO("[ASTAR CONTROLLER] cur_goal_dis = %lf, path_size = %zu", cur_goal_dis, path.size());

  taken_global_goal_ = false;
  if(begin_index == 0 && (cur_goal_dis < 3.5 || // co_->sbpl_max_distance
     co_->fixpattern_path->Length() < co_->front_safe_check_dis ||
     path.size() <= 5)) {  
    if (IsGoalFootprintSafe(0.5, 0.0, global_goal_)) {
      planner_goal_ = global_goal_;
      taken_global_goal_ = true;
      planner_goal_index_ = (int)path.size() - 1;
      GAUSSIAN_INFO("[ASTAR CONTROLLER] taking global_goal_ as planner_goal_");
      return true;
    } else {
      double acc_dis = 0.0;
      std::vector<geometry_msgs::PoseStamped>::iterator it;
      for (it = path.end() - 1; it >= path.begin() + 2; it -= 2) {
        if (IsGoalFootprintSafe(0.5 , 0.3, *it)) {
          planner_goal_ = *it;
          planner_goal_.header.frame_id = co_->global_frame;
          planner_goal_index_ = it - path.begin();
          GAUSSIAN_INFO("[ASTAR CONTROLLER] taking global_goal_ as planner_goal_");
          return true;
        }
        acc_dis += PoseStampedDistance(*it, *(it - 2));
        if (acc_dis > cur_goal_dis) {
          GAUSSIAN_WARN("[ASTAR CONTROLLER] Cur_goal_dis = %lf < 2.5m, but GetAStarGoal failed", cur_goal_dis);
          return false;
        }
      }
    }
  } else {
    bool cross_obstacle = false;
    double dis_accu = 0.0;
    int goal_index = -1;
    double goal_safe_dis_a, goal_safe_dis_b;
    int i, j;
    for (j = 0; j < 4; ++j) {
      cross_obstacle = false;
      dis_accu = 0.0;
      goal_index = -1;
      goal_safe_dis_a = co_->goal_safe_dis_a - j * 0.2;
      goal_safe_dis_b = co_->goal_safe_dis_b;
      GAUSSIAN_INFO("[ASTAR CONTROLLER] get astar goal, round: %d", j);
      for (i = begin_index; i < path.size(); i += 2) {
        if (i > begin_index) dis_accu += PoseStampedDistance(path.at(i), path.at(i - 2));
        // we must enforce cross obstacle within front_safe_check_dis range
//        if (!cross_obstacle && dis_accu <= co_->front_safe_check_dis) continue;
        if (dis_accu <= goal_safe_dis_a) continue;
        if (PoseStampedDistance(cur_pose, path.at(i)) <= goal_safe_dis_a) continue;
//        GAUSSIAN_INFO("[ASTAR CONTROLLER] dis_accu = %lf", dis_accu);
        double x = path[i].pose.position.x;
        double y = path[i].pose.position.y;
        double yaw = tf::getYaw(path[i].pose.orientation);
        if (footprint_checker_->CircleCenterCost(x, y, yaw, co_->circle_center_points, extend_x, extend_y) < 0 ||
             !IsGoalFootprintSafe(goal_safe_dis_a, goal_safe_dis_b, path[i])) {
           cross_obstacle = true;
//           GAUSSIAN_INFO("[ASTAR CONTROLLER] path[%d] not safe", i);
           continue;
        }
        goal_index = i;
        break;
      }
      if (goal_index != -1 || (!cross_obstacle && i >= path.size())) {
        if (i >= path.size()) goal_index = path.size() - 1;
        break;
      } 
    }
    if (goal_index == -1 || goal_index >= path.size()) {
      GAUSSIAN_WARN("[ASTAR CONTROLLER] GetAStarGoal failed, cost: %lf secs", GetTimeInSeconds() - start);
      return false;
    }
    planner_goal_ = path[goal_index];
    planner_goal_.header.frame_id = co_->global_frame;
    planner_goal_index_ = goal_index;
  }
  astar_goal_pub_.publish(planner_goal_);
  GAUSSIAN_INFO("[ASTAR CONTROLLER] GetAStarGoal cost: %lf secs", GetTimeInSeconds() - start);
  GAUSSIAN_INFO("[ASTAR CONTROLLER] planner_goal_index_: %d", planner_goal_index_);
  return true;
}

bool AStarController::GetAStarTempGoal(geometry_msgs::PoseStamped& goal_pose, double offset_dis) {
  GAUSSIAN_INFO("[ASTAR CONTROLLER] GetAStarTempGoal!");
  bool cross_obstacle = false;
  double dis_accu = 0.0;
  int goal_index = -1;
  double goal_safe_dis_a = 0.4;
  double goal_safe_dis_b = 0.3;
  int i;
  std::vector<geometry_msgs::PoseStamped> path = co_->fixpattern_path->GeometryPath();
  for (i = 0; i < path.size(); i += 1) {
    if (i > 0) dis_accu += PoseStampedDistance(path.at(i), path.at(i - 1));
    // we must enforce cross obstacle within front_safe_check_dis range
    if (dis_accu <= offset_dis) continue;
    double x = path[i].pose.position.x;
    double y = path[i].pose.position.y;
    double yaw = tf::getYaw(path[i].pose.orientation);
    if (footprint_checker_->CircleCenterCost(x, y, yaw, co_->circle_center_points, 0.0, 0.0) < 0 ||
         !IsGoalFootprintSafe(goal_safe_dis_a, goal_safe_dis_b, path[i])) {
       cross_obstacle = true;
       continue;
    }
    goal_index = i;
    break;
  }
  if (!cross_obstacle && i >= path.size()) {
    goal_index = path.size() - 1;
  } 

  if (goal_index == -1 || goal_index >= path.size()) {
    GAUSSIAN_WARN("[ASTAR CONTROLLER] GetAStarTempGoal failed");
    return false;
  }
  goal_pose = path[goal_index];
  goal_pose.header.frame_id = co_->global_frame;
//  sbpl_goal_pub_.publish(goal_pose);
  GAUSSIAN_INFO("[ASTAR CONTROLLER] temp planner_goal_index_: %d", goal_index);
  return true;
}

void AStarController::PublishPlan(const ros::Publisher& pub, const std::vector<geometry_msgs::PoseStamped>& plan) {
  // create a message for the plan
  nav_msgs::Path gui_path;
  gui_path.poses.resize(plan.size());

  if (!plan.empty()) {
    gui_path.header.frame_id = plan[0].header.frame_id;
    gui_path.header.stamp = plan[0].header.stamp;
  }

  for (unsigned int i = 0; i < plan.size(); i++) {
    gui_path.poses[i] = plan[i];
  }

  // publish
  pub.publish(gui_path);
}

void AStarController::PublishMovebaseStatus(unsigned int status_index) {
  // create a message for the plan
  std_msgs::UInt32 status_msg;
  status_msg.data = status_index;
  // publish
  move_base_status_pub_.publish(status_msg);
}

void AStarController::PublishHeadingGoal() {
  // publish
  heading_goal_pub_.publish(global_goal_);
}

void AStarController::PublishGoalReached(geometry_msgs::PoseStamped goal_pose) {
  // publish
  goal_reached_pub_.publish(goal_pose);
}

void AStarController::SampleInitialPath(std::vector<geometry_msgs::PoseStamped>* planner_plan,
                                        std::vector<fixpattern_path::PathPoint>& fix_path) {
    geometry_msgs::PoseStamped pre_pose = planner_plan->front();
    fix_path.clear();
    fix_path.push_back(fixpattern_path::GeometryPoseToPathPoint(planner_plan->front().pose));
    double yaw_diff;
    double acc_dis = 0.0;
    int acc_count = 0;
    for (int i = 1; i < planner_plan->size() - 1; ++i, ++acc_count) {
      acc_dis += PoseStampedDistance(planner_plan->at(i - 1), planner_plan->at(i)); 
      yaw_diff = angles::shortest_angular_distance(tf::getYaw(pre_pose.pose.orientation), tf::getYaw(planner_plan->at(i).pose.orientation));
      if (acc_dis > co_->init_path_sample_dis || fabs(yaw_diff) > co_->init_path_sample_yaw || acc_count % 5 == 0 ) {
        acc_dis = 0.0;
        acc_count = 0;
        fix_path.push_back(fixpattern_path::GeometryPoseToPathPoint(planner_plan->at(i).pose));
        pre_pose = planner_plan->at(i);
      }
    }
    fix_path.push_back(fixpattern_path::GeometryPoseToPathPoint(planner_plan->back().pose));
}

bool AStarController::GetAStarInitialPath(const geometry_msgs::PoseStamped& global_start, const geometry_msgs::PoseStamped& global_goal) {
  if (!co_->astar_global_planner->makePlan(global_start, global_goal, *planner_plan_) || planner_plan_->empty()) {
    GAUSSIAN_ERROR("[ASTAR CONTROLLER] InitialPath: a_planner failed to find a plan to point (%.2f, %.2f)", global_goal.pose.position.x, global_goal.pose.position.y);
    PublishAStarExtendPose(global_start, global_goal);
    return false;
  } else {
    std::vector<fixpattern_path::PathPoint> fix_path;
    SampleInitialPath(planner_plan_, fix_path);
    co_->fixpattern_path->set_fix_path(global_start, fix_path, true); 

    // check fix_path is safe: if not, get  goal on path and switch to PLANNING state 
    
    if (RecheckFixPath(global_start, true)) {
      GAUSSIAN_INFO("[ASTAR CONTROLLER] InitialPath: recheck fixpath successed!");
    } else {
      GAUSSIAN_WARN("[ASTAR CONTROLLER] InitialPath: recheck fixpath failed!");
    }
    
    gotInitPlan_ = true;

    std::vector<geometry_msgs::PoseStamped> plan = co_->fixpattern_path->GeometryPath();
    for (auto&& p : plan) {  // NOLINT
      p.header.frame_id = co_->global_frame;
      p.header.stamp = ros::Time::now();
    }
    PublishPlan(fixpattern_pub_, plan);

    GAUSSIAN_INFO("[ASTAR CONTROLLER] InitialPath: After set_fix_path size = %d", (int)plan.size());
    return true;
  }
}

bool AStarController::RecheckFixPath(const geometry_msgs::PoseStamped& global_start, bool using_static_costmap) {
  // set footprint_checker costmap is static or not
  footprint_checker_->setStaticCostmap(controller_costmap_ros_, using_static_costmap);
  // check fix_path is safe: if not, get goal on path and switch to PLANNING state 
  int try_count = 10;	
  while(--try_count > 0) {
    if (CheckFixPathFrontSafe(co_->fixpattern_path->GeometryPath(), co_->fixpattern_path->Length(), 0.0, co_->init_path_circle_center_extend_y) < co_->fixpattern_path->Length() - 0.30, 0) {
      GetAStarGoal(global_start, 0.0, co_->init_path_circle_center_extend_y, obstacle_index_);
      GetAStarStart(co_->fixpattern_path->Length(), 0.0, co_->init_path_circle_center_extend_y, obstacle_index_);
      GAUSSIAN_ERROR("[ASTAR CONTROLLER] RecheckFixPath: path_not safe, start to recheck and replan");

      fixpattern_path::Path temp_sbpl_path;
      // set static costmap in first planning
      co_->sbpl_global_planner->setStaticCosmap(using_static_costmap);
      if (!co_->sbpl_global_planner->makePlan(planner_start_, planner_goal_, *planner_plan_, temp_sbpl_path, false, false)
          || planner_plan_->empty()) {
        GAUSSIAN_ERROR("[ASTAR CONTROLLER] RecheckFixPath: sbpl failed to find a plan to point (%.2f, %.2f)", planner_goal_.pose.position.x, planner_goal_.pose.position.y);
      } else {
        co_->fixpattern_path->insert_middle_path(temp_sbpl_path.path(), planner_start_, planner_goal_);
        GAUSSIAN_INFO("[ASTAR CONTROLLER] RecheckFixPath: after inserting sbpl path, fix_path length = %lf", co_->fixpattern_path->Length());
      }
    } else {
      GAUSSIAN_INFO("[ASTAR CONTROLLER] RecheckFixPath: check fixpath safe, updated successed!");
      return true;
    }
  } 

  if (try_count <= 0) {
    GAUSSIAN_WARN("[ASTAR CONTROLLER] RecheckFixPath: check fixpath not safe then 10 times, return false!");
    return false;
  }	
}

bool AStarController::HandleSwitchingPath(geometry_msgs::PoseStamped current_position, bool switch_directly) {
  if (switch_path_ && switch_directly) {
    co_->fixpattern_path->set_path(front_path_.path(), false, false); 
    return true;
  }
  if (!switch_path_) return false; 
  if (front_path_.path().size() < 30 || front_path_.Length() < 1.0 || 
      PoseStampedDistance(planner_start_, current_position) > 1.5 || 
      PoseStampedDistance(front_goal_, current_position) < 1.5) {
    switch_path_ = false;
    return false;
  }

  fixpattern_path::PathPoint start_pose = fixpattern_path::GeometryPoseToPathPoint(current_position.pose); 
  front_path_.Prune(start_pose, 0.8, M_PI / 2.0, false);
  double dis_diff, yaw_diff;
  // handle corner point diffrent from others
  if (co_->fixpattern_path->path().front().corner_struct.corner_point) {
    if (front_path_.CheckCurPoseOnPath(start_pose, co_->switch_corner_dis_diff, co_->switch_corner_yaw_diff)) {
      if (CheckFixPathFrontSafe(front_path_.GeometryPath(), co_->front_safe_check_dis, 0.0, co_->init_path_circle_center_extend_y) > 2.0 &&
          front_path_.Length() - co_->fixpattern_path->Length() < 0.0 &&
          ++origin_path_safe_cnt_ > 2) {
        co_->fixpattern_path->set_fix_path(current_position, front_path_.path(), false, true); 
        first_run_controller_flag_ = true;
        switch_path_ = false;
        GAUSSIAN_INFO("[ASTAR CONTROLLER] corner: switch origin path as fix path");
      }
    } else {
      GAUSSIAN_WARN("[ASTAR CONTROLLER] corner: rotate too much, abandon this front path");
      switch_path_ = false;
    }
  } else {
    if (CheckFixPathFrontSafe(front_path_.GeometryPath(), co_->front_safe_check_dis, 0.0, co_->init_path_circle_center_extend_y) > 2.0 &&
        front_path_.Length() - co_->fixpattern_path->Length() < 0.0) {
      if (front_path_.CheckCurPoseOnPath(start_pose, co_->switch_normal_dis_diff, co_->switch_normal_yaw_diff)) { 
        co_->fixpattern_path->set_fix_path(current_position, front_path_.path(), false, false); 
        switch_path_ = false;
        GAUSSIAN_INFO("[ASTAR CONTROLLER] switch origin path as fix path");
      } else {
        bool get_bezier_plan = false;
        std::vector<fixpattern_path::PathPoint> bezier_path;
        if (front_goal_index_ > 0 && front_goal_index_ < front_path_.GeometryPath().size()) {
          geometry_msgs::PoseStamped goal = front_path_.GeometryPath().at(front_goal_index_);
          if (MakeBezierPlan(&bezier_path, current_position, goal, false)) {
            astar_path_.set_bezier_path(current_position, bezier_path, false);
            front_path_.insert_begin_path(astar_path_.path(), current_position, goal, false, M_PI / 3.0);
            get_bezier_plan = true;
          }
        }
        if(get_bezier_plan && ++origin_path_safe_cnt_ > 10 &&
           CheckFixPathFrontSafe(front_path_.GeometryPath(), co_->front_safe_check_dis, 0.0, co_->init_path_circle_center_extend_y) > 2.0 &&
           front_path_.Length() - co_->fixpattern_path->Length() < 0.0) {
          co_->fixpattern_path->set_fix_path(current_position, front_path_.path(), false, false); 
          first_run_controller_flag_ = true;
          switch_path_ = false;
          GAUSSIAN_INFO("[ASTAR CONTROLLER] switch origin path as fix path");
        }
      } 
      switch_path_ = false;
    }
  }
  return true;
}
bool AStarController::CheckProtector(geometry_msgs::PoseStamped& current_position, bool detect_front_protector) {
  autoscrubber_services::CheckProtectorStatus protector_status;
  check_protector_client_.call(protector_status);
  bool b_protector_status = protector_status.response.protector_status.protect_status;
  GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] check protector status = %d", b_protector_status);
  bool b_front_protector_detected = true;
  if (b_protector_status && detect_front_protector) {
    unsigned int protector_value = protector_status.response.protector_status.protect_value;
    b_front_protector_detected = false;
    for (int i = 0; i < co_->front_protector_list.size(); ++i) {
      GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] check protector status bit[%d]", co_->front_protector_list.at(i));
      if (protector_value & (1 << co_->front_protector_list.at(i))) {
        b_front_protector_detected = true;
        GAUSSIAN_ERROR("[FIXPATTERN CONTROLLER] check protector status bit[%d] detected!!!", co_->front_protector_list.at(i));
      }
    }
    GAUSSIAN_WARN("[FIXPATTERN CONTROLLER] check front protector true or false: %d", b_front_protector_detected);
    if (b_front_protector_detected) {
      GAUSSIAN_ERROR("[FIXPATTERN CONTROLLER] check front protector true, we'll handlegoingback ");
      HandleGoingBack(current_position, co_->backward_check_dis + 0.05);
    }
  }
  return b_protector_status && b_front_protector_detected;
}

bool AStarController::LocalizationRecovery() {
  if (!localization_valid_) {
    GAUSSIAN_WARN("[ASTAR CONTROLLER] localization failed! Recovery now by inplace_rotating");
    autoscrubber_services::StartRotate start_rotate;
    autoscrubber_services::StopRotate stop_rotate;
    autoscrubber_services::CheckRotate check_rotate;
    start_rotate.request.rotateAngle.data = 360;
    start_rotate_client_.call(start_rotate);
    do {
      // TODO (lizhen) Alarm Here!
      PublishMovebaseStatus(E_LOCATION_INVALID);
      check_rotate_client_.call(check_rotate);
      usleep(100000);   //100ms 10hz
    } while (!check_rotate.response.isFinished.data && !localization_valid_ && env_->run_flag);

    stop_rotate_client_.call(stop_rotate);
    PublishZeroVelocity();
    usleep(500000);
  }
  return localization_valid_;
}

bool AStarController::HeadingChargingGoal(const geometry_msgs::PoseStamped& charging_goal) {
  double cur_goal_dis; 
  geometry_msgs::Twist cmd_vel;
  tf::Stamped<tf::Pose> global_pose;
  geometry_msgs::PoseStamped cur_pos;
  ros::Rate control_rate(co_->controller_frequency);
  while (!CheckProtector(cur_pos, false) && env_->run_flag) {
    // get curent position
    controller_costmap_ros_->getRobotPose(global_pose);
    tf::poseStampedTFToMsg(global_pose, cur_pos);
    if (PoseStampedDistance(cur_pos, charging_goal) > 0.03) {
      // make sure that we send the velocity command to the base
      GAUSSIAN_INFO("[ASTAR CONTROLLER] charging: going back!");
      cmd_vel.linear.x = -0.1;
      cmd_vel.angular.z = 0.0;
      co_->vel_pub->publish(cmd_vel);
    } else {
      GAUSSIAN_INFO("[ASTAR CONTROLLER] charging: going back reached!");
      PublishZeroVelocity();
      break;
    }
    last_valid_control_ = ros::Time::now();
    control_rate.sleep();
  }
  return true;
}
bool AStarController::HandleGoingBack(geometry_msgs::PoseStamped& current_position, double backward_dis) {
  geometry_msgs::Twist cmd_vel;
  geometry_msgs::PoseStamped cur_pos = current_position;
  if (backward_dis <= 0.01) {
    backward_dis = co_->backward_check_dis;
  }
  // check if need backward
  ros::Time end_time = ros::Time::now() + ros::Duration(co_->stop_duration / 5);
  bool need_backward = true;
  ros::Rate r(10);
  while (ros::Time::now() < end_time && env_->run_flag) {
    if (!NeedBackward(cur_pos, backward_dis)) {
      need_backward = false;
      break;
    }
    GAUSSIAN_INFO("[ASTAR CONTROLLER] Need Backward, Publish Zero Vel");
    // stop first, and set last_valid_control_
    PublishZeroVelocity();
    last_valid_control_ = ros::Time::now();
    r.sleep();
  }
  ros::Rate control_rate(co_->controller_frequency);
  tf::Stamped<tf::Pose> global_pose;
//  while (need_backward && NeedBackward(cur_pos, 0.15) && CanBackward(0.25)) {
  while (env_->run_flag && need_backward && NeedBackward(cur_pos, backward_dis + 0.05) && CanBackward(backward_dis + 0.15)) {
    GAUSSIAN_INFO("[ASTAR CONTROLLER] going back");
    // get curent position
    controller_costmap_ros_->getRobotPose(global_pose);
    tf::poseStampedTFToMsg(global_pose, cur_pos);

    // make sure that we send the velocity command to the base
    cmd_vel.linear.x = -0.1;
    cmd_vel.angular.z = 0.0;
    co_->vel_pub->publish(cmd_vel);

    last_valid_control_ = ros::Time::now();
    control_rate.sleep();
  }
  current_position = cur_pos;
  return need_backward;
}

bool AStarController::EscapeRecovery(geometry_msgs::PoseStamped current_pos) {
  GAUSSIAN_INFO("[FIXPATTERN CONTROLLER] Handle Recovery!");
  bool ret = false;
  geometry_msgs::PoseStamped goal_pos;
  double target_yaw = footprint_checker_->RecoveryCircleCost(current_pos, footprint_spec_, &goal_pos);
  if(target_yaw < M_PI * 2.0) {
    double target_dis = PoseStampedDistance(current_pos, goal_pos);
    if (RotateToYaw(target_yaw)) {
      GAUSSIAN_INFO("rotate to yaw done, next going forward dis = %lf", target_dis);
//      if (CanForward(target_dis)) {
      if (GoingForward(target_dis / 3.5)) {
        GAUSSIAN_INFO("GoingForward done");
        return true;
      }
    }
  }

  GAUSSIAN_INFO("[ASTAR CONTROLLER] RotateRecovery: Cannot Rotate to target_yaw: %lf, try to go forward", target_yaw);
  // go forward, if can
  if (GoingForward(0.20)) {
    return true;
  }

  // backward...
  if (GoingBackward(0.20)) {
    return true;
  } else {
    return false;
  }
}

void AStarController::UpdateRecoveryYaw(geometry_msgs::PoseStamped current_position) {
  double current_yaw = tf::getYaw(current_position.pose.orientation);
  rotate_recovery_target_yaw_[0] = current_yaw + M_PI_4;
  rotate_recovery_target_yaw_[1] = current_yaw + M_PI_2;
  rotate_recovery_target_yaw_[2] = current_yaw;
  rotate_recovery_target_yaw_[3] = current_yaw - M_PI_4;
  rotate_recovery_target_yaw_[4] = current_yaw - M_PI_2;
  rotate_recovery_target_yaw_[5] = current_yaw - M_PI_4;
  rotate_recovery_target_yaw_[6] = current_yaw;
/*
  rotate_recovery_target_yaw_[6] = current_yaw + M_PI;
  rotate_recovery_target_yaw_[7] = current_yaw + M_PI / 4.0;
  rotate_recovery_target_yaw_[8] = current_yaw + M_PI / 4.0;
  rotate_recovery_target_yaw_[9] = current_yaw - M_PI;
  rotate_recovery_target_yaw_[10] = current_yaw - M_PI / 4.0;
  rotate_recovery_target_yaw_[11] = current_yaw - M_PI / 4.0;
  rotate_recovery_target_yaw_[12] = current_yaw + M_PI;
*/
}


bool AStarController::CanRotate(double x, double y, double yaw, int dir) {
  // only check 0.4 radian, ignore current footprint
  unsigned int rotate_failure_time = 0;
  for (int i = 1; i <= 4; ++i) {
    if (footprint_checker_->CircleCenterCost(x, y, yaw + dir * 0.1 * i,
                                             co_->circle_center_points, 0.0, 0.0) < 0.0) {
      GAUSSIAN_INFO("[ASTAR CONTROLLER] CanRotate: false, yaw: %lf, dir: %d", yaw, dir);
      ++rotate_failure_time;
    }
  }
  if (rotate_failure_time >= 3) {
    GAUSSIAN_WARN("[ASTAR CONTROLLER] CanRotate: false");
    return false;
  } else { 
    GAUSSIAN_INFO("[ASTAR CONTROLLER] CanRotate: true");
    return true;
  }
}

bool AStarController::RotateToYaw(double target_yaw) {
  tf::Stamped<tf::Pose> global_pose;
  geometry_msgs::PoseStamped current_position;

  controller_costmap_ros_->getRobotPose(global_pose);
  tf::poseStampedTFToMsg(global_pose, current_position);
  double x = current_position.pose.position.x;
  double y = current_position.pose.position.y;
  double yaw = tf::getYaw(current_position.pose.orientation);
  double angle_diff = angles::shortest_angular_distance(yaw, target_yaw);

  geometry_msgs::Twist cmd_vel;
  cmd_vel.linear.x = 0.0;
  cmd_vel.linear.y = 0.0;

  ros::Rate r(co_->controller_frequency);
  while (fabs(angle_diff) > 0.1 && CanRotate(x, y, yaw, angle_diff > 0 ? 1 : -1) && env_->run_flag) {
//  while (fabs(angle_diff) > 0.1 && env_->run_flag) {
    GAUSSIAN_INFO("rotate to yaw: cur_yaw = %lf, target_yaw = %lf, yaw_diff = %lf",yaw ,target_yaw, angle_diff);
    cmd_vel.angular.z = angle_diff > 0 ? 0.3 : -0.3;
    co_->vel_pub->publish(cmd_vel);
    last_valid_control_ = ros::Time::now();

    controller_costmap_ros_->getRobotPose(global_pose);
    tf::poseStampedTFToMsg(global_pose, current_position);
    x = current_position.pose.position.x;
    y = current_position.pose.position.y;
    yaw = tf::getYaw(current_position.pose.orientation);
    angle_diff = angles::shortest_angular_distance(yaw, target_yaw);

    r.sleep();
  }

  if (fabs(angle_diff) > 0.1) {
    return false;
  } else {
    return true;
  }
}

bool AStarController::CanBackward(double distance) {
  tf::Stamped<tf::Pose> global_pose;
  controller_costmap_ros_->getRobotPose(global_pose);
  geometry_msgs::PoseStamped current_position;
  tf::poseStampedTFToMsg(global_pose, current_position);

  double x = current_position.pose.position.x;
  double y = current_position.pose.position.y;
  double yaw = tf::getYaw(current_position.pose.orientation);
  double resolution = controller_costmap_ros_->getCostmap()->getResolution();
  int num_step = distance / resolution;
  int skip_num = 3; 

  std::vector<geometry_msgs::PoseStamped> path;
  // ignore current footprint
  for (int i = skip_num; i <= num_step + skip_num - 1; ++i) {
    double new_x = x - i * resolution * cos(yaw);
    double new_y = y - i * resolution * sin(yaw);
    if (footprint_checker_->CircleCenterCost(new_x, new_y, yaw,
                                             co_->backward_center_points, 0.0, 0.0) < -1.1) {
      GAUSSIAN_WARN("[ASTAR CONTROLLER] CanBackward: false");
      return false;
    }
  }
  GAUSSIAN_INFO("[ASTAR CONTROLLER] CanBackward: true");
  return true;
}

bool AStarController::GoingBackward(double distance) {
  if (!CanBackward(0.20)) return false;

  double backward_time = distance / 0.1;
  ros::Time end_time = ros::Time::now() + ros::Duration(backward_time);

  geometry_msgs::Twist cmd_vel;
  cmd_vel.linear.x = -0.1;
  cmd_vel.linear.y = 0.0;
  cmd_vel.angular.z = 0.0;

  ros::Rate r(co_->controller_frequency);
  while (ros::Time::now() < end_time && CanBackward(0.20) && env_->run_flag) {
    co_->vel_pub->publish(cmd_vel);
    last_valid_control_ = ros::Time::now();

    r.sleep();
  }
  return true;
}

bool AStarController::CanForward(double distance) {
  tf::Stamped<tf::Pose> global_pose;
  controller_costmap_ros_->getRobotPose(global_pose);
  geometry_msgs::PoseStamped current_position;
  tf::poseStampedTFToMsg(global_pose, current_position);

  double x = current_position.pose.position.x;
  double y = current_position.pose.position.y;
  double yaw = tf::getYaw(current_position.pose.orientation);
  double resolution = controller_costmap_ros_->getCostmap()->getResolution();
  int num_step = distance / resolution;
  int skip_num = 2;

  std::vector<geometry_msgs::PoseStamped> path;
  // ignore current footprint
  for (int i = skip_num; i <= num_step + skip_num - 1; ++i) {
    double new_x = x + i * resolution * cos(yaw);
    double new_y = y + i * resolution * sin(yaw);
    if (footprint_checker_->CircleCenterCost(new_x, new_y, yaw,
                                             co_->circle_center_points, 0.0, 0.0) < 0) {
      GAUSSIAN_INFO("[ASTAR CONTROLLER] CanForward: false");
      return false;
    }
  }
  GAUSSIAN_INFO("[ASTAR CONTROLLER] CanForward: true");
  return true;
}

bool AStarController::GoingForward(double distance) {
  if (!CanForward(0.05)) return false;

  double forward_time = distance / 0.1;
  ros::Time end_time = ros::Time::now() + ros::Duration(forward_time);

  geometry_msgs::Twist cmd_vel;
  cmd_vel.linear.x = 0.15;
  cmd_vel.linear.y = 0.0;
  cmd_vel.angular.z = 0.0;

  ros::Rate r(co_->controller_frequency);
  while (ros::Time::now() < end_time && CanForward(0.05) && env_->run_flag) {
 // while (ros::Time::now() < end_time) {
    co_->vel_pub->publish(cmd_vel);
    last_valid_control_ = ros::Time::now();

    r.sleep();
  }
  return true;
}

bool AStarController::RotateRecovery() {
  // rotate to previous direction
  tf::Stamped<tf::Pose> global_pose;
  controller_costmap_ros_->getRobotPose(global_pose);
  geometry_msgs::PoseStamped current_position;
  tf::poseStampedTFToMsg(global_pose, current_position);
  
  GAUSSIAN_INFO("[ASTAR CONTROLLER] RotateRecovery: try_recovery_times_ = %d", try_recovery_times_);
  if (try_recovery_times_ == 0) {
    UpdateRecoveryYaw(current_position);
  } else if (try_recovery_times_ > 6) {
    ++try_recovery_times_;
    return true;
  }

  double current_yaw = tf::getYaw(current_position.pose.orientation);
  double target_yaw = rotate_recovery_target_yaw_[try_recovery_times_];
  double theta_sim_granularity = target_yaw > current_yaw ? 0.1 : -0.1;
  GAUSSIAN_INFO("[ASTAR CONTROLLER] RotateRecovery: current_yaw: %lf, target_yaw: %lf", current_yaw, target_yaw);

  int num_step = M_PI / 4.0 / fabs(theta_sim_granularity);
  if (num_step == 0) num_step = 1;

  bool footprint_safe = true;
  // ignore current footprint
  for (int i = 1; i <= num_step; ++i) {
    double sample_yaw = angles::normalize_angle(current_yaw + i * theta_sim_granularity);
    if (footprint_checker_->CircleCenterCost(current_position.pose.position.x, current_position.pose.position.y,
                                             sample_yaw, co_->circle_center_points, 0.0, 0.0) < 0) {
      footprint_safe = false;
      break;
    }
  }
  if (footprint_safe) {
    GAUSSIAN_INFO("[ASTAR CONTROLLER] RotateRecovery: footprint_safe, try to rotate");
    if (RotateToYaw(target_yaw)) {
      return true;
    }
  } 
  {
    GAUSSIAN_INFO("[ASTAR CONTROLLER] RotateRecovery: Cannot Rotate to target_yaw: %lf, try to go forward", target_yaw);
    // go forward, if can
    if (GoingForward(0.20)) {
      return true;
    }

    // backward...
    if (GoingBackward(0.20)) {
      return true;
    } else {
      return false;
    }
  }

/*
  if (rotate_recovery_dir_ == 0) rotate_recovery_dir_ = 1;
  double target_yaw = angles::normalize_angle(yaw + rotate_recovery_dir_ * M_PI / 6);
  double theta_sim_granularity = rotate_recovery_dir_ > 0 ? 0.1 : -0.1;

  int num_step = M_PI / 6 / fabs(theta_sim_granularity);
  if (num_step == 0) num_step = 1;

  bool footprint_safe = true;
  // ignore current footprint
  for (int i = 1; i <= num_step; ++i) {
    double sample_yaw = angles::normalize_angle(yaw + i * theta_sim_granularity);
    if (footprint_checker_->CircleCenterCost(current_position.pose.position.x, current_position.pose.position.y,
                                             sample_yaw, co_->circle_center_points, 0.0, 0.0) < 0) {
      footprint_safe = false;
      break;
    }
  }
  if (footprint_safe) {
    GAUSSIAN_INFO("[ASTAR CONTROLLER] RotateToYaw, yaw: %lf, target_yaw: %lf", yaw, target_yaw);
    RotateToYaw(target_yaw);
    return true;
  }

  rotate_failure_times_++;
  // can rotate only if at least one direction is safe
  if (rotate_failure_times_ < 2) {
    // if cannot rotate to previous direction, change direction
    rotate_recovery_dir_ *= -1;
    target_yaw = angles::normalize_angle(yaw + rotate_recovery_dir_ * M_PI / 6);
    theta_sim_granularity = rotate_recovery_dir_ > 0 ? 0.1 : -0.1;

    footprint_safe = true;
    // ignore current footprint
    for (int i = 1; i <= num_step; ++i) {
      double sample_yaw = angles::normalize_angle(yaw + i * theta_sim_granularity);
      if (footprint_checker_->CircleCenterCost(current_position.pose.position.x, current_position.pose.position.y,
                                               sample_yaw, co_->circle_center_points, 0.0, 0.0) < 0) {
        footprint_safe = false;
        break;
      }
    }
    if (footprint_safe) {
      GAUSSIAN_INFO("[ASTAR CONTROLLER] RotateToYaw, yaw: %lf, target_yaw: %lf", yaw, target_yaw);
      RotateToYaw(target_yaw);
      return true;
    }

    rotate_failure_times_++;
  }

  // we should reset recovery_dir_ and failure_times_ here
  rotate_recovery_dir_ = 0;
  rotate_failure_times_ = 0;

  // backward...
  if (GoingBackward(0.25)) {
    return true;
  }

  // go forward, if can
  if (GoingForward(0.25)) {
    return true;
  }

  // at last... have a try to going back?
  geometry_msgs::Twist cmd_vel;
  cmd_vel.linear.x = -0.1;
  cmd_vel.linear.y = 0.0;
  cmd_vel.angular.z = 0.0;
  co_->vel_pub->publish(cmd_vel);
  return true;
*/
}

};  // namespace service_robot
