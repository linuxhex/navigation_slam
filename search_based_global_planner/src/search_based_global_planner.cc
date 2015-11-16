/* Copyright(C) Gaussian Automation. All rights reserved.
 */

#include "search_based_global_planner/search_based_global_planner.h"

#include <nav_msgs/Path.h>
#include <costmap_2d/inflation_layer.h>
#ifdef DEBUG
#include <gperftools/profiler.h>
#endif

#include "search_based_global_planner/utils.h"

#define COMPUTEKEY(entry) (entry)->ComputeKey(eps_, env_->GetHeuristic((entry)->x, (entry)->y))

namespace search_based_global_planner {

SearchBasedGlobalPlanner::SearchBasedGlobalPlanner() : initialized_(false) { }

SearchBasedGlobalPlanner::~SearchBasedGlobalPlanner() { }

double GetNumberFromXMLRPC(XmlRpc::XmlRpcValue& value, const std::string& full_param_name) {
  // Make sure that the value we're looking at is either a double or an int.
  if (value.getType() != XmlRpc::XmlRpcValue::TypeInt &&
      value.getType() != XmlRpc::XmlRpcValue::TypeDouble) {
    std::string& value_string = value;
    ROS_FATAL("Values in the circle_center specification (param %s) must be numbers. Found value %s.",
              full_param_name.c_str(), value_string.c_str() );
    throw std::runtime_error("Values in the circle_center specification must be numbers");
  }
  return value.getType() == XmlRpc::XmlRpcValue::TypeInt ? static_cast<int>(value) : static_cast<double>(value);
}

void ReadCircleCenterFromXMLRPC(XmlRpc::XmlRpcValue& circle_center_xmlrpc, const std::string& full_param_name, std::vector<XYPoint>* points) {
  // Make sure we have an array of at least 3 elements.
  if (circle_center_xmlrpc.getType() != XmlRpc::XmlRpcValue::TypeArray || circle_center_xmlrpc.size() < 1) {
    ROS_FATAL("The circle_center must be specified as list of lists on the parameter server, %s was specified as %s",
              full_param_name.c_str(), std::string(circle_center_xmlrpc).c_str());
    throw std::runtime_error("The circle_center must be specified as list of lists on the parameter server with at least 1 points eg: [[x1, y1], [x2, y2], ..., [xn, yn]]");
  }

  XYPoint pt;

  for (int i = 0; i < circle_center_xmlrpc.size(); ++i) {
    // Make sure each element of the list is an array of size 2. (x and y coordinates)
    XmlRpc::XmlRpcValue point = circle_center_xmlrpc[ i ];
    if (point.getType() != XmlRpc::XmlRpcValue::TypeArray ||
        point.size() != 2) {
      ROS_FATAL("The circle_center (parameter %s) must be specified as list of lists on the parameter server eg: [[x1, y1], [x2, y2], ..., [xn, yn]], but this spec is not of that form.", full_param_name.c_str());
      throw std::runtime_error( "The circle_center must be specified as list of lists on the parameter server eg: [[x1, y1], [x2, y2], ..., [xn, yn]], but this spec is not of that form");
    }

    pt.x = GetNumberFromXMLRPC(point[0], full_param_name);
    pt.y = GetNumberFromXMLRPC(point[1], full_param_name);

    points->push_back(pt);
  }
}

bool SearchBasedGlobalPlanner::ReadCircleCenterFromParams(ros::NodeHandle& nh, std::vector<XYPoint>* points) {
  std::string full_param_name;

  if (nh.searchParam("circle_center", full_param_name)) {
    XmlRpc::XmlRpcValue circle_center_xmlrpc;
    nh.getParam(full_param_name, circle_center_xmlrpc);
    if (circle_center_xmlrpc.getType() == XmlRpc::XmlRpcValue::TypeArray) {
      ReadCircleCenterFromXMLRPC(circle_center_xmlrpc, full_param_name, points);
      return true;
    } else {
      ROS_ERROR("[SEARCH BASED GLOBAL PLANNER] circle_center param's type is not Array!");
      return false;
    }
  }
}

void SearchBasedGlobalPlanner::initialize(std::string name, costmap_2d::Costmap2DROS* costmap_ros) {
  if (!initialized_) {
    initialized_ = true;
    ros::NodeHandle private_nh("~/" + name);
    plan_pub_ = private_nh.advertise<nav_msgs::Path>("plan", 1);
    costmap_ros_ = costmap_ros;

    private_nh.param("allocated_time", allocated_time_, 4.0);
    private_nh.param("initial_epsilon", initial_epsilon_, 3.0);
    private_nh.param("force_scratch_limit", force_scratch_limit_, 500);

    double nominalvel_mpersec, timetoturn45degsinplace_secs;
    private_nh.param("nominalvel_mpersecs", nominalvel_mpersec, 0.4);
    private_nh.param("timetoturn45degsinplace_secs", timetoturn45degsinplace_secs, 0.6);

    // get circle_center
    std::vector<XYPoint> circle_center_point;
    if (!ReadCircleCenterFromParams(private_nh, &circle_center_point)) {
      exit(1);
    }

    // get footprint
    std::vector<geometry_msgs::Point> footprint = costmap_ros_->getRobotFootprint();
    std::vector<XYPoint> footprint_point;
    for (const auto& p : footprint) {
      footprint_point.push_back(XYPoint(p.x, p.y));
    }

    resolution_ = costmap_ros_->getCostmap()->getResolution();

    // check if the costmap has an inflation layer
    // Warning: footprint updates after initialization are not supported here
    unsigned char cost_possibly_circumscribed_thresh = 0;
    for (std::vector<boost::shared_ptr<costmap_2d::Layer> >::const_iterator layer = costmap_ros_->getLayeredCostmap()->getPlugins()->begin();
        layer != costmap_ros_->getLayeredCostmap()->getPlugins()->end();
        ++layer) {
      boost::shared_ptr<costmap_2d::InflationLayer> inflation_layer = boost::dynamic_pointer_cast<costmap_2d::InflationLayer>(*layer);
      if (!inflation_layer) continue;

      cost_possibly_circumscribed_thresh = inflation_layer->computeCost(costmap_ros_->getLayeredCostmap()->getCircumscribedRadius() / resolution_);
    }

    int lethal_cost = 20;
    private_nh.param("lethal_cost", lethal_cost, 20);
    lethal_cost_ = static_cast<unsigned char>(lethal_cost);
    inscribed_inflated_cost_ = lethal_cost_ - 1;
    cost_multiplier_ = static_cast<unsigned char>(costmap_2d::INSCRIBED_INFLATED_OBSTACLE / inscribed_inflated_cost_ + 1);
    cost_possibly_circumscribed_thresh = TransformCostmapCost(cost_possibly_circumscribed_thresh);
    ROS_INFO("[SEARCH BASED GLOBAL PLANNER] cost_possibly_circumscribed_thresh: %d", static_cast<int>(cost_possibly_circumscribed_thresh));

    const int num_of_angles = 16;
    const int num_of_prims_per_angle = 7;
    int forward_cost_mult, forward_and_turn_cost_mult, turn_in_place_cost_mult;
    private_nh.param("forward_cost_mult", forward_cost_mult, 1);
    private_nh.param("forward_and_turn_cost_mult", forward_and_turn_cost_mult, 2);
    private_nh.param("turn_in_place_cost_mult", turn_in_place_cost_mult, 50);

    private_nh.param("map_size", map_size_, 400);

    unsigned int size_x = costmap_ros_->getCostmap()->getSizeInCellsX();
    unsigned int size_y = costmap_ros_->getCostmap()->getSizeInCellsY();
    size_dir_ = num_of_angles;

    iteration_ = 0;
    environment_iteration_ = 0;

    if (size_x < map_size_ || size_y < map_size_) {
      ROS_ERROR("[SEARCH BASED GLOBAL PLANNER] map_size is too big");
      exit(1);
    }
    size_x = size_y = map_size_;

    env_ = new Environment(size_x, size_y, resolution_, lethal_cost_, inscribed_inflated_cost_,
                           cost_possibly_circumscribed_thresh, nominalvel_mpersec,
                           timetoturn45degsinplace_secs, footprint_point, circle_center_point,
                           num_of_angles, num_of_prims_per_angle, forward_cost_mult,
                           forward_and_turn_cost_mult, turn_in_place_cost_mult);

    need_to_reinitialize_environment_ = true;
    ROS_INFO("[SEARCH BASED GLOBAL PLANNER] Search Based Global Planner initialized");
  } else {
    ROS_WARN("[SEARCH BASED GLOBAL PLANNER] This planner has already been initialized,"
             " you can't call it twice, doing nothing");
  }
}

void SearchBasedGlobalPlanner::PublishPlan(const std::vector<geometry_msgs::PoseStamped>& plan) {
  if (!initialized_) {
    ROS_ERROR("[SEARCH BASED GLOBAL PLANNER] publishPlan This planner has not been initialized yet,"
              " but it is being used, please call initialize() before use");
    return;
  }

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
  plan_pub_.publish(gui_path);
}

void SearchBasedGlobalPlanner::RecomputeRHSVal(EnvironmentEntry3D* entry) {
  // rhs(s) = min... refer to paper
  std::vector<EnvironmentEntry3D*> succ_entries;
  std::vector<int> succ_costs;
  env_->GetSuccs(entry, &succ_entries, &succ_costs);
  for (int i = 0; i < succ_entries.size(); ++i) {
    EnvironmentEntry3D* succ_entry = succ_entries[i];
    if (succ_entry->visited_iteration != environment_iteration_) continue;
    if (entry->rhs > succ_costs[i] + succ_entry->g) {
      entry->rhs = succ_costs[i] + succ_entry->g;
      // update parent entry
      entry->best_next_entry = succ_entry;
    }
  }
}

void SearchBasedGlobalPlanner::UpdateSetMembership(EnvironmentEntry3D* entry) {
  if (entry->rhs != entry->g) {
    if (entry->closed_iteration != iteration_) {
      COMPUTEKEY(entry);
      if (PTRHEAP_OK != open_.contain(entry)) {
        // ROS_INFO("[SEARCH BASED GLOBAL PLANNER] push to open_ (%d %d %d)", entry->x, entry->y, entry->theta);
        open_.push(entry);
      } else {
        // ROS_INFO("[SEARCH BASED GLOBAL PLANNER] update (%d %d %d)", entry->x, entry->y, entry->theta);
        open_.adjust(entry);
      }
    } else {
      inconsist_.insert(entry);
    }
  } else {
    if (PTRHEAP_OK == open_.contain(entry)) {
      // ROS_INFO("[SEARCH BASED GLOBAL PLANNER] erase from open_ (%d %d %d)", entry->x, entry->y, entry->theta);
      open_.erase(entry);
    }
  }
}

void SearchBasedGlobalPlanner::UpdateStateOfUnderConsist(EnvironmentEntry3D* entry) {
  std::vector<EnvironmentEntry3D*> pred_entries;
  std::vector<int> costs;

  env_->GetPreds(entry, &pred_entries, &costs);
  for (int i = 0; i < pred_entries.size(); ++i) {
    EnvironmentEntry3D* pred_entry = pred_entries[i];
    // if entry was not visited before: entry->g = INFINITECOST
    if (pred_entry->visited_iteration != environment_iteration_) {
      pred_entry->g = INFINITECOST;
      pred_entry->visited_iteration = environment_iteration_;
    }

    if (pred_entry->best_next_entry == entry) {
      RecomputeRHSVal(pred_entry);
      UpdateSetMembership(pred_entry);
    }
  }
}

void SearchBasedGlobalPlanner::UpdateStateOfOverConsist(EnvironmentEntry3D* entry) {
  std::vector<int> costs;
  std::vector<EnvironmentEntry3D*> pred_entries;

  env_->GetPreds(entry, &pred_entries, &costs);
  for (int i = 0; i < pred_entries.size(); ++i) {
    EnvironmentEntry3D* pred_entry = pred_entries[i];
    // if entry was not visited before: entry->g = INFINITECOST
    if (pred_entry->visited_iteration != environment_iteration_) {
      pred_entry->g = INFINITECOST;
      pred_entry->visited_iteration = environment_iteration_;
    }

    if (pred_entry->rhs > costs[i] + entry->g) {
      // optimization: assume entry is the best
      pred_entry->rhs = costs[i] + entry->g;
      // update parent entry
      pred_entry->best_next_entry = entry;

      UpdateSetMembership(pred_entry);
    }
  }
}

bool SearchBasedGlobalPlanner::ComputeOrImprovePath() {
#ifdef DEBUG
  size_t max_open_size = 0;
#endif
  // get start_entry_list
  std::vector<EnvironmentEntry3D*> start_entry_list;
  if (broader_start_and_goal_) {
    std::vector<int> delta_x{-2, -1, 0, 1, 2};
    std::vector<int> delta_y{-2, -1, 0, 1, 2};
    int start_x = start_entry_->x;
    int start_y = start_entry_->y;
    uint8_t start_theta = start_entry_->theta;
    for (const auto& i : delta_x) {
      for (const auto& j : delta_y) {
        if (i != 0 && j != 0) continue;
        EnvironmentEntry3D* entry = env_->GetEnvEntry(start_x + i, start_y + j, start_theta);
        if (entry) start_entry_list.push_back(entry);
      }
    }
  } else {
    start_entry_list.push_back(start_entry_);
  }
  first_met_entry_ = start_entry_;
  // begin compute
  EnvironmentEntry3D* min_entry = open_.top();
  while (min_entry != NULL && GetTimeInSeconds() - start_time_ < allocated_time_ ) {
    bool search_over = false;
    for (const auto& start_entry : start_entry_list) {
      if (COMPUTEKEY(min_entry) >= COMPUTEKEY(start_entry) && start_entry->rhs == start_entry->g) {
        first_met_entry_ = start_entry;
        search_over = true;
        break;
      }
    }
    if (search_over) break;
#ifdef DEBUG
    if (open_.size() > max_open_size) max_open_size = open_.size();
#endif
    // remove state s with the minimum key from OPEN
    // ROS_INFO("[SEARCH BASED GLOBAL PLANNER] expand entry in open_ (%d %d %d)", min_entry->x, min_entry->y, min_entry->theta);
    open_.pop();
    if (min_entry->g > min_entry->rhs) {
      min_entry->g = min_entry->rhs;
      // push to CLOSED
      min_entry->closed_iteration = iteration_;
      // for all s' from Pred(s) UpdateState(s')
      UpdateStateOfOverConsist(min_entry);
    } else {
      min_entry->g = INFINITECOST;
      UpdateSetMembership(min_entry);
      UpdateStateOfUnderConsist(min_entry);
    }

    min_entry = open_.top();
  }
#ifdef DEBUG
  ROS_INFO("[SEARCH BASED GLOBAL PLANNER] max_open_size: %d", (int)max_open_size);
#endif

  if (first_met_entry_->rhs == INFINITECOST && open_.empty()) {
    ROS_ERROR("[SEARCH BASED GLOBAL PLANNER] solution does not exist: search exited because heap is empty");
    return false;
  } else if (!open_.empty() &&
             (min_entry->key < COMPUTEKEY(first_met_entry_) || first_met_entry_->rhs > first_met_entry_->g)) {
    ROS_ERROR("[SEARCH BASED GLOBAL PLANNER] search exited because it ran out of time");
    return false;
  } else if (first_met_entry_->rhs == INFINITECOST && !open_.empty()) {
    ROS_ERROR("[SEARCH BASED GLOBAL PLANNER] solution does not exist: search exited because all candidates for expansion have infinite heuristics");
    return false;
  } else {
    ROS_INFO("search exited with a solution for eps=%.3f", eps_);
    return true;
  }
}

void SearchBasedGlobalPlanner::GetEntryPath(std::vector<EnvironmentEntry3D*>* entry_path) {
  std::vector<int> costs;
  std::vector<EnvironmentEntry3D*> succ_entries;

  EnvironmentEntry3D* entry = first_met_entry_;

  entry_path->push_back(entry);

  while (*entry != *goal_entry_) {
    if (entry->best_next_entry == NULL) {
      ROS_ERROR("[SEARCH BASED GLOBAL PLANNER] path does not exist since best_next_entry == NULL");
      break;
    }
    if (entry->rhs == INFINITECOST) {
      ROS_ERROR("[SEARCH BASED GLOBAL PLANNER] path does not exist since rhs == INFINITECOST");
      break;
    }

    if (entry->g < entry->rhs) {
      ROS_ERROR("[SEARCH BASED GLOBAL PLANNER] underconsistent entry on the path");
      return;
    }

    entry = entry->best_next_entry;

    entry_path->push_back(entry);
  }

  if (*entry != *goal_entry_) {
    ROS_ERROR("[SEARCH BASED GLOBAL PLANNER] Failed to GetSearchPath");
    entry_path->clear();
  }
}

void SearchBasedGlobalPlanner::GetPointPathFromEntryPath(const std::vector<EnvironmentEntry3D*>& entry_path,
                                                         std::vector<XYThetaPoint>* point_path,
                                                         std::vector<IntermPointStruct>* path_info) {
  if (entry_path.size() == 0) return;

  std::vector<EnvironmentEntry3D*> succ_entries;
  std::vector<int> costs;
  std::vector<Action*> actions;

  point_path->clear();
  path_info->clear();

  for (unsigned int pind = 0; pind < entry_path.size() - 1; ++pind) {
    EnvironmentEntry3D* source_entry = entry_path.at(pind);
    EnvironmentEntry3D* target_entry = entry_path.at(pind + 1);

    // get successors and pick the target via the cheapest action
    succ_entries.clear();
    costs.clear();
    actions.clear();
    env_->GetSuccs(source_entry, &succ_entries, &costs, &actions);

    int best_cost = INFINITECOST;
    int best_index = -1;
    for (unsigned int sind = 0; sind < succ_entries.size(); ++sind) {
      if (*succ_entries[sind] == *target_entry && costs[sind] <= best_cost) {
        best_cost = costs[sind];
        best_index = sind;
      }
    }
    if (best_index == -1) {
      if (broader_start_and_goal_) {
        for (const auto& entry : goal_entry_list_)
          if (*source_entry == *entry && *target_entry == *goal_entry_) return;
      }
      ROS_ERROR("[SEARCH BASED GLOBAL PLANNER] successor not found for transition");
      point_path->clear();
      path_info->clear();
      return;
    }

    // now push in the actual path
    double source_x = DISCXY2CONT(source_entry->x, resolution_);
    double source_y = DISCXY2CONT(source_entry->y, resolution_);

    for (int ipind = 0; ipind < static_cast<int>(actions[best_index]->interm_pts.size()) - 1; ++ipind) {
      // translate appropriately
      XYThetaPoint interm_point = actions[best_index]->interm_pts[ipind];
      interm_point.x += source_x;
      interm_point.y += source_y;

      // store
      point_path->push_back(interm_point);
      path_info->push_back(actions[best_index]->interm_struct[ipind]);
    }
  }
}

void SearchBasedGlobalPlanner::ReInitializeSearchEnvironment() {
  env_->ReInitialize();

  open_.clear();
  inconsist_.clear();

  eps_ = initial_epsilon_;
  epsilon_satisfied_ = INFINITECOST;

  environment_iteration_++;

  goal_entry_list_.clear();

  // put goal_entry_ to open_, entries around goal_entry_ too
  if (broader_start_and_goal_) {
    std::vector<int> delta_x{-3, -2, -1, 0, 1, 2, 3};
    std::vector<int> delta_y{-3, -2, -1, 0, 1, 2, 3};
    std::vector<int> delta_theta{-1, 0, 1};
    int goal_x = goal_entry_->x;
    int goal_y = goal_entry_->y;
    uint8_t goal_theta = goal_entry_->theta;
    for (const auto& i : delta_x) {
      for (const auto& j : delta_y) {
        for (const auto& k : delta_theta) {
          // if (i != 0 && j != 0) continue;

          EnvironmentEntry3D* entry = env_->GetEnvEntry(goal_x + i, goal_y + j, goal_theta + k);
          if (!entry) continue;
          goal_entry_list_.push_back(entry);

          entry->rhs = 0;
          entry->visited_iteration = environment_iteration_;
          if (i != 0 || j != 0) entry->best_next_entry = goal_entry_;
          COMPUTEKEY(entry);
          open_.push(entry);
        }
      }
    }
  } else {
    goal_entry_->rhs = 0;
    goal_entry_->visited_iteration = environment_iteration_;
    COMPUTEKEY(goal_entry_);
    open_.push(goal_entry_);
  }

  need_to_reinitialize_environment_ = false;
}

bool SearchBasedGlobalPlanner::search(std::vector<XYThetaPoint>* point_path, std::vector<IntermPointStruct>* path_info) {
  start_time_ = GetTimeInSeconds();

  if (need_to_reinitialize_environment_) {
    ReInitializeSearchEnvironment();
  }

  double before_heuristic = GetTimeInSeconds();
  env_->EnsureHeuristicsUpdated();
  ROS_INFO("[SEARCH BASED GLOBAL PLANNER] EnsureHeuristicsUpdated cost %lf seconds", GetTimeInSeconds() - before_heuristic);

  while (epsilon_satisfied_ > 1.0 && GetTimeInSeconds() - start_time_ < allocated_time_) {
    if (fabs(epsilon_satisfied_ - eps_) < 0.000001) {
      // epsilon_satisfied_ != eps_ when first come to here
      if (eps_ > 1.0) eps_ -= 1.0;
      if (eps_ < 1.0) eps_ = 1.0;

      // new iteration - CLOSED = empty
      iteration_++;
    }

    // move states from INCONS into OPEN
    for (const auto& e : inconsist_) {
      // e shouldn't be in open_, because we'll check if e in open_ when push
      // to inconsist_, if in, we'll remove it from open_ first
      open_.push(e);
    }
    inconsist_.clear();

    // update the priorities for all s from OPEN according to key(s)
    for (auto it = open_.begin(); it != open_.end(); ++it)
      COMPUTEKEY(*it);
    open_.make_heap();

    double start_time = GetTimeInSeconds();
    if (ComputeOrImprovePath()) {
      epsilon_satisfied_ = eps_;
    }
    ROS_INFO("[SEARCH BASED GLOBAL PLANNER] ComputeOrImprovePath cost %lf seconds", GetTimeInSeconds() - start_time);

    if (first_met_entry_->rhs == INFINITECOST) break;
  }

  if (first_met_entry_->rhs == INFINITECOST || epsilon_satisfied_ == INFINITECOST) {
    ROS_ERROR("[SEARCH BASED GLOBAL PLANNER] cannot find a solution");
    return false;
  } else {
    std::vector<EnvironmentEntry3D*> entry_path;
    GetEntryPath(&entry_path);
    GetPointPathFromEntryPath(entry_path, point_path, path_info);
    ROS_INFO("[SEARCH BASED GLOBAL PLANNER] solution found");
    return true;
  }
}

bool SearchBasedGlobalPlanner::CostsChanged(const std::vector<XYCell>& changed_cells) {
  if (need_to_reinitialize_environment_ || iteration_ == 0)
    return true;

  EnvironmentEntry3D* entry = NULL;
  std::vector<EnvironmentEntry3D*> affected_entries;
  char* exist = (char*)calloc(map_size_ * map_size_ * size_dir_, sizeof(char));
  if (!exist) {
    ROS_ERROR("[SEARCH BASED GLOBAL PLANNER] allocate memory for exist in CostsChanged failed");
    exit(0);
  }

  double start_time = GetTimeInSeconds();
  for (const auto& cell : changed_cells) {
    // now iterate over all states that could potentially be affected
    std::vector<XYThetaCell>& affected_pred_cells = env_->GetAffectedPredCells();
    for (auto affected_cell : affected_pred_cells) {
      // translate to correct for the offset
      affected_cell.x = affected_cell.x + cell.x;
      affected_cell.y = affected_cell.y + cell.y;

      entry = env_->GetEnvEntry(affected_cell.x, affected_cell.y, affected_cell.theta);
      if (!entry) continue;

      int index = affected_cell.theta + affected_cell.x * size_dir_ + affected_cell.y * map_size_ * size_dir_;
      if (exist[index] == 1) continue;
      exist[index] = 1;

      // insert to affected_entries
      affected_entries.push_back(entry);
    }
  }
  // don't forget to free exist
  free(exist);
  ROS_INFO("[SEARCH BASED GLOBAL PLANNER] CostsChanged cost %lf seconds, changed_cells.size() %d, affected_entries.size() %d",
           GetTimeInSeconds() - start_time, (int)changed_cells.size(), (int)affected_entries.size());

  if (affected_entries.size() <= 0) return true;

  // update preds of changed edges
  if (affected_entries.size() > map_size_ * map_size_ * size_dir_ / 10 || affected_entries.size() > force_scratch_limit_) {
    need_to_reinitialize_environment_ = true;
  }

  for (const auto& entry : affected_entries) {
    if (entry->visited_iteration == environment_iteration_) {
      RecomputeRHSVal(entry);
      UpdateSetMembership(entry);
    }
  }

  // reset eps for which we know a path was computed
  eps_ = initial_epsilon_;
  epsilon_satisfied_ = INFINITECOST;

  return true;
}

unsigned char SearchBasedGlobalPlanner::TransformCostmapCost(unsigned char cost) {
  if (cost == costmap_2d::LETHAL_OBSTACLE) {
    return lethal_cost_;
  } else if (cost == costmap_2d::INSCRIBED_INFLATED_OBSTACLE) {
    return inscribed_inflated_cost_;
  } else if (cost == 0 || cost == costmap_2d::NO_INFORMATION) {
    return 0;
  } else {
    return static_cast<unsigned char>(cost / cost_multiplier_ + 0.5);
  }
}

bool SearchBasedGlobalPlanner::makePlan(geometry_msgs::PoseStamped start,
                                        geometry_msgs::PoseStamped goal,
                                        std::vector<geometry_msgs::PoseStamped>& plan,
                                        fixpattern_path::Path& path, bool broader_start_and_goal, bool extend_path) {
#ifdef DEBUG
  ProfilerStart("sbpl.prof");
#endif
  if (!initialized_) {
    ROS_ERROR("[SEARCH BASED GLOBAL PLANNER] SearchBasedGlobalPlanner is not initialized");
    return false;
  }

  plan.clear();

  broader_start_and_goal_ = broader_start_and_goal;

  double theta_start = 2 * atan2(start.pose.orientation.z, start.pose.orientation.w);
  double theta_goal = 2 * atan2(goal.pose.orientation.z, goal.pose.orientation.w);

  // // get robot pos in cells
  // tf::Stamped<tf::Pose> global_pose;
  // costmap_ros_->getRobotPose(global_pose);
  // geometry_msgs::PoseStamped tmp_pos;
  // tf::poseStampedTFToMsg(global_pose, tmp_pos);
  unsigned int cell_x, cell_y;
  if (!costmap_ros_->getCostmap()->worldToMap(start.pose.position.x,
                            start.pose.position.y, cell_x, cell_y)) {
    ROS_ERROR("[SBPL LATTICE PLANNER] world to map failed");
    return false;
  }

  // get lower left point of sbpl map
  unsigned int start_cell_x = 0;
  unsigned int start_cell_y = 0;
  if (cell_x > map_size_ / 2 && cell_x <= costmap_ros_->getCostmap()->getSizeInCellsX() - map_size_ / 2) {
    start_cell_x = cell_x - map_size_ / 2;
  } else if (cell_x > costmap_ros_->getCostmap()->getSizeInCellsX() - map_size_ / 2) {
    start_cell_x = costmap_ros_->getCostmap()->getSizeInCellsX() - map_size_;
  }
  if (cell_y > map_size_ / 2 && cell_y <= costmap_ros_->getCostmap()->getSizeInCellsY() - map_size_ / 2) {
    start_cell_y = cell_y - map_size_ / 2;
  } else if (cell_y > costmap_ros_->getCostmap()->getSizeInCellsY() - map_size_ / 2) {
    start_cell_y = costmap_ros_->getCostmap()->getSizeInCellsY() - map_size_;
  }

  double start_x, start_y;
  costmap_ros_->getCostmap()->mapToWorld(start_cell_x, start_cell_y, start_x, start_y);
  start_x -= resolution_ / 2.0;
  start_y -= resolution_ / 2.0;

  // set start and goal point, we have to set goal first in case computing
  // heuristic values when set start
  EnvironmentEntry3D* last_goal_entry = goal_entry_;
  EnvironmentEntry3D* last_start_entry = start_entry_;
  goal_entry_ = env_->SetGoal(goal.pose.position.x - start_x,
                             goal.pose.position.y - start_y, theta_goal);
  start_entry_ = env_->SetStart(start.pose.position.x - start_x,
                               start.pose.position.y - start_y, theta_start);
  if (!start_entry_ || !goal_entry_)
    return false;
  if (last_start_entry != start_entry_) {
    // current solution may be invalid
    eps_ = initial_epsilon_;
    epsilon_satisfied_ = INFINITECOST;
  }
  if (last_goal_entry != goal_entry_) {
    // if goal changed, we want to ReInitializeSearchEnvironment
    // plz refer to sbpl project
    need_to_reinitialize_environment_ = true;
  }

  // we want to enforce reintialization temporarily
  // need_to_reinitialize_environment_ = true;

  ROS_INFO("[SEARCH BASED GLOBAL PLANNER] receive goal (%d %d %d), start (%d %d %d)",
           goal_entry_->x, goal_entry_->y, goal_entry_->theta, start_entry_->x, start_entry_->y, start_entry_->theta);

  // update costs that are changed
  std::vector<XYCell> changed_cells;

  for (unsigned int ix = 0; ix < map_size_; ++ix) {
    for (unsigned int iy = 0; iy < map_size_; ++iy) {
      unsigned char old_cost = env_->GetCost(ix, iy);
      unsigned char new_cost = TransformCostmapCost(costmap_ros_->getCostmap()->getCost(ix + start_cell_x, iy + start_cell_y));

      if (old_cost == new_cost) continue;

      env_->UpdateCost(ix, iy, new_cost);

      XYCell cell(ix, iy);
      changed_cells.push_back(cell);
    }
  }

  double before_costs_changed = GetTimeInSeconds();
  if (!changed_cells.empty())
    CostsChanged(changed_cells);
  ROS_INFO("[SEARCH BASED GLOBAL PLANNER] CostsChanged cost %lf seconds", GetTimeInSeconds() - before_costs_changed);

  // compute plan
  std::vector<XYThetaPoint> point_path;
  std::vector<IntermPointStruct> path_info;
  search(&point_path, &path_info);

  if (point_path.size() == 0)
    return false;

  // fill plan
  ros::Time plan_time = ros::Time::now();
  for (unsigned int i = 0; i < point_path.size(); ++i) {
    geometry_msgs::PoseStamped pose;
    pose.header.stamp = plan_time;
    pose.header.frame_id = costmap_ros_->getGlobalFrameID();

    pose.pose.position.x = point_path[i].x + start_x;
    pose.pose.position.y = point_path[i].y + start_y;
    pose.pose.position.z = start.pose.position.z;

    tf::Quaternion temp;
    temp.setRPY(0, 0, point_path[i].theta);
    pose.pose.orientation.x = temp.getX();
    pose.pose.orientation.y = temp.getY();
    pose.pose.orientation.z = temp.getZ();
    pose.pose.orientation.w = temp.getW();

    plan.push_back(pose);
  }
  // if (!broader_start_and_goal_) {
    plan.push_back(goal);
  // }

  // publish the plan
  PublishPlan(plan);

  // assign to fixpattern_path::Path
  std::vector<fixpattern_path::PathPoint> tmp_path;
  for (unsigned int i = 0; i < plan.size() - 1; ++i) {
    if (path_info[i].is_corner) {
      unsigned int corner_size = 1;
      for (unsigned int j = i + 1; j < plan.size() - 1; ++j) {
        if (path_info[j].is_corner) corner_size++;
        else break;
      }
      unsigned int corner_end_index = i + (corner_size - 1);
      if (corner_size >= 27) {
        for (unsigned int j = i; j <= corner_end_index; ++j) {
          fixpattern_path::PathPoint point = fixpattern_path::GeometryPoseToPathPoint(plan[j].pose);
          point.radius = path_info[j].radius;
          point.corner_struct.corner_point = true;
          point.corner_struct.theta_out = path_info[corner_end_index].theta_out;
          point.corner_struct.rotate_direction = path_info[corner_end_index].rotate_direction;
          tmp_path.push_back(point);
          ROS_INFO("[SEARCH BASED GLOBAL PLANNER] corner_point index: %d, size: %d, real theta_out: %lf, dir: %d", j, (int)path_info.size(), path_info[j].theta_out, path_info[j].rotate_direction);
        }
      } else {
        for (unsigned int j = i; j <= corner_end_index; ++j) {
          fixpattern_path::PathPoint point = fixpattern_path::GeometryPoseToPathPoint(plan[j].pose);
          point.radius = path_info[j].radius;
          point.corner_struct.corner_point = false;
          point.corner_struct.theta_out = 0.0;
          point.corner_struct.rotate_direction = 0;
          tmp_path.push_back(point);
        }
      }
      i = corner_end_index;
    } else {
      fixpattern_path::PathPoint point = fixpattern_path::GeometryPoseToPathPoint(plan[i].pose);
      point.radius = path_info[i].radius;
      point.corner_struct.corner_point = false;
      point.corner_struct.theta_out = 0.0;
      point.corner_struct.rotate_direction = 0;
      tmp_path.push_back(point);
    }
  }
  int corner_size = 0;
  for (int i = 0; i < tmp_path.size(); ++i) {
    if (tmp_path[i].corner_struct.corner_point) corner_size++;
  }
  ROS_INFO("[SBPL] total_size: %d, corner_size: %d", (int)tmp_path.size(), corner_size);

  // mark points before and after corner as corner_point
  for (unsigned int i = 0; i < tmp_path.size(); ++i) {
    if (tmp_path[i].corner_struct.corner_point) {
      unsigned int begin = i;
      unsigned int end = i;
      double theta_out = tmp_path[i].corner_struct.theta_out;
      int rotate_direction = tmp_path[i].corner_struct.rotate_direction;
      double dis_accu = 0.0;
      while (begin > 0 && dis_accu < fixpattern_path::Path::MIN_BEFORE_CORNER_LENGTH) {
        dis_accu += tmp_path[begin].DistanceToPoint(tmp_path[begin - 1]);
        begin--;
      }
      dis_accu = 0.0;
      // we don't want to use MIN_AFTER_CORNER_LENGTH directly, as sbpl plan are always curve after corner, so we want it to be short
      while (end < tmp_path.size() - 1 && dis_accu < fixpattern_path::Path::MIN_AFTER_CORNER_LENGTH * 0.25) {
        dis_accu += tmp_path[end].DistanceToPoint(tmp_path[end + 1]);
        end++;
      }
      for (unsigned int j = begin; j <= end; ++j) {
        tmp_path[j].corner_struct.corner_point = true;
        tmp_path[j].corner_struct.theta_out = theta_out;
        tmp_path[j].corner_struct.rotate_direction = rotate_direction;
      }
      i = end;
    }
  }
  // TODO(chenkan): what if two corner are too close?

  // if (!broader_start_and_goal_) {
    fixpattern_path::PathPoint point = fixpattern_path::GeometryPoseToPathPoint(plan.back().pose);
    point.radius = 0.5;
    point.corner_struct.corner_point = false;
    point.corner_struct.theta_out = 0.0;
    tmp_path.push_back(point);
  // }

  if (extend_path) {
    fixpattern_path::Path temp_sbpl_path;
    temp_sbpl_path.set_sbpl_path(tmp_path);
    path.ExtendPath(temp_sbpl_path.path());
  } else {
    path.set_sbpl_path(tmp_path);
  }

  corner_size = 0;
  for (int i = 0; i < tmp_path.size(); ++i) {
    if (tmp_path[i].corner_struct.corner_point) corner_size++;
  }
  ROS_INFO("[SBPL] total_size: %d, corner_size: %d", (int)tmp_path.size(), corner_size);

#ifdef DEBUG
  ProfilerStop();
#endif
  return true;
}

};  // namespace search_based_global_planner