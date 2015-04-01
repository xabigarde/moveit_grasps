/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2015, University of Colorado, Boulder
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Univ of CO, Boulder nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Dave Coleman <dave@dav.ee>
   Desc:   Filters grasps based on kinematic feasibility and collision
*/

#include <moveit_grasps/grasp_filter.h>
#include <moveit/transforms/transforms.h>
#include <moveit/collision_detection/collision_tools.h>

// Conversions
#include <eigen_conversions/eigen_msg.h>

// Parameter loading
#include <rviz_visual_tools/ros_param_utilities.h>

namespace
{
bool ikCallbackFnAdapter(moveit::core::RobotState *state, const moveit::core::JointModelGroup *group,
                         const moveit::core::GroupStateValidityCallbackFn &constraint,
                         const geometry_msgs::Pose &, const std::vector<double> &ik_sol, moveit_msgs::MoveItErrorCodes &error_code)
{
  const std::vector<unsigned int> &bij = group->getKinematicsSolverJointBijection();
  std::vector<double> solution(bij.size());
  for (std::size_t i = 0 ; i < bij.size() ; ++i)
    solution[bij[i]] = ik_sol[i];
  if (constraint(state, group, &solution[0]))
    error_code.val = moveit_msgs::MoveItErrorCodes::SUCCESS;
  else
    error_code.val = moveit_msgs::MoveItErrorCodes::NO_IK_SOLUTION;
  return true;
}
}

namespace moveit_grasps
{

// Constructor
GraspFilter::GraspFilter( robot_state::RobotStatePtr robot_state,
                          moveit_visual_tools::MoveItVisualToolsPtr& visual_tools )
  : visual_tools_(visual_tools)
  , nh_("~/filter")
  , secondary_collision_checking_(false) // TODO remove this featuer
{
  // Make a copy of the robot state so that we are sure outside influence does not break our grasp filter
  robot_state_.reset(new moveit::core::RobotState(*robot_state));
  robot_state_->update(); // make sure transforms are computed

  // Load visulization settings
  const std::string parent_name = "filter"; // for namespacing logging messages
  rviz_visual_tools::getBoolParameter(parent_name, nh_, "collision_verbose", collision_verbose_);
  rviz_visual_tools::getDoubleParameter(parent_name, nh_, "collision_verbose_speed", collision_verbose_speed_);
  rviz_visual_tools::getBoolParameter(parent_name, nh_, "show_filtered_grasps", show_filtered_grasps_);
  rviz_visual_tools::getBoolParameter(parent_name, nh_, "show_filtered_arm_solutions", show_filtered_arm_solutions_);
  rviz_visual_tools::getDoubleParameter(parent_name, nh_, "show_filtered_arm_solutions_speed", show_filtered_arm_solutions_speed_);
  rviz_visual_tools::getDoubleParameter(parent_name, nh_, "show_filtered_arm_solutions_pregrasp_speed", show_filtered_arm_solutions_pregrasp_speed_);


  ROS_DEBUG_STREAM_NAMED("filter","Loaded grasp filter");
}

std::vector<GraspCandidatePtr> GraspFilter::convertToGraspCandidatePtrs(const std::vector<moveit_msgs::Grasp>& possible_grasps,
                                                                        const GraspDataPtr grasp_data)
{
  std::vector<GraspCandidatePtr> candidates;

  for (std::size_t i = 0; i < possible_grasps.size(); ++i)
  {
    candidates.push_back(GraspCandidatePtr(new GraspCandidate(possible_grasps[i], grasp_data)));
  }
  return candidates;
}

std::size_t GraspFilter::filterGrasps(std::vector<GraspCandidatePtr>& grasp_candidates,
                                      planning_scene_monitor::PlanningSceneMonitorPtr planning_scene_monitor,
                                      const robot_model::JointModelGroup* arm_jmg,
                                      bool filter_pregrasp,
                                      bool verbose, bool verbose_if_failed, bool collision_verbose)
{
  // -----------------------------------------------------------------------------------------------
  // Error check
  if( grasp_candidates.empty() )
  {
    ROS_ERROR_NAMED("filter","Unable to filter grasps because vector is empty");
    return false;
  }
  if (!filter_pregrasp)
    ROS_WARN_STREAM_NAMED("filter","Not filtering pre-grasp - GraspCandidate may have bad data");

  // Override verbose settings with yaml settings if necessary
  if (collision_verbose_)
    collision_verbose = collision_verbose_;

  // -----------------------------------------------------------------------------------------------
  // Get the solver timeout from kinematics.yaml
  solver_timeout_ = arm_jmg->getDefaultIKTimeout();
  ROS_DEBUG_STREAM_NAMED("grasp_filter","Grasp filter IK timeout " << solver_timeout_);

  // -----------------------------------------------------------------------------------------------
  // Choose how many degrees of freedom
  num_variables_ = arm_jmg->getVariableCount();
  ROS_DEBUG_STREAM_NAMED("grasp_filter","Solver for " << num_variables_ << " degrees of freedom");

  // -----------------------------------------------------------------------------------------------
  // Get the end effector joint model group
  if (arm_jmg->getAttachedEndEffectorNames().size() == 0)
  {
    ROS_ERROR_STREAM_NAMED("grasp_filter","No end effectors attached to this arm");
    return false;
  }
  else if (arm_jmg->getAttachedEndEffectorNames().size() > 1)
  {
    ROS_ERROR_STREAM_NAMED("grasp_filter","More than one end effectors attached to this arm");
    return false;
  }

  // Try to filter grasps not in verbose mode
  std::size_t remaining_grasps = filterGraspsHelper(grasp_candidates, planning_scene_monitor, arm_jmg, filter_pregrasp,
                                                    verbose, collision_verbose);
  if (remaining_grasps == 0)
  {
    ROS_ERROR_STREAM_NAMED("filter","IK filter unable to find any valid grasps! Re-running in verbose mode");
    if (verbose_if_failed)
    {
      verbose = true;
      remaining_grasps = filterGraspsHelper(grasp_candidates, planning_scene_monitor, arm_jmg, filter_pregrasp, verbose,
                                            collision_verbose);
    }
  }

  // Visualize valid grasps as arrows with cartesian path as well
  if (show_filtered_grasps_)
  {
    ROS_INFO_STREAM_NAMED("filter", "Showing filtered grasps");
    visualizeGrasps(grasp_candidates, arm_jmg);
  }

  // Visualize valid grasp as arm positions
  if (show_filtered_arm_solutions_)
  {
    ROS_INFO_STREAM_NAMED("filter", "Showing filtered arm solutions");
    visualizeCandidateGrasps(grasp_candidates);
  }

  return remaining_grasps;
}

std::size_t GraspFilter::filterGraspsByPlane(std::vector<GraspCandidatePtr>& grasp_candidates,
                                             Eigen::Affine3d filter_pose,
                                             grasp_parallel_plane plane, int direction)
{
  ROS_DEBUG_STREAM_NAMED("filter_by_plane","Starting with " << grasp_candidates.size() << " grasps");

  std::size_t num_grasps_filtered = 0;

  Eigen::Affine3d grasp_pose;
  Eigen::Vector3d grasp_position;

  for (std::size_t i = 0; i < grasp_candidates.size(); i++)
  {
    // get grasp translation in filter pose CS
    grasp_pose = visual_tools_->convertPose(grasp_candidates[i]->grasp_.grasp_pose.pose);
    grasp_position = filter_pose.inverse() * grasp_pose.translation();
    
    // filter grasps by cutting plane
    switch(plane)
    {
      case XY:
        if (direction == -1 && grasp_position(2) < 0)
          grasp_candidates[i]->grasp_filtered_by_obstruction_ = true;
        else if (direction == 1 && grasp_position(2) > 0)
          grasp_candidates[i]->grasp_filtered_by_obstruction_ = true;
        break;
      case XZ:
        if (direction == -1 && grasp_position(1) < 0)
          grasp_candidates[i]->grasp_filtered_by_obstruction_ = true;
        else if (direction == 1 && grasp_position(1) > 0)
          grasp_candidates[i]->grasp_filtered_by_obstruction_ = true;
        break;
      case YZ:
        if (direction == -1 && grasp_position(0) < 0)
          grasp_candidates[i]->grasp_filtered_by_obstruction_ = true;
        else if (direction == 1 && grasp_position(0) > 0)
          grasp_candidates[i]->grasp_filtered_by_obstruction_ = true;
        break;
      default:
        ROS_WARN_STREAM_NAMED("filter_by_plane","plane not specified correctly");
        break;
    }
    
    if (grasp_candidates[i]->grasp_filtered_by_obstruction_ == true)
      num_grasps_filtered++;

  }
  ROS_DEBUG_STREAM_NAMED("filter_by_plane","Number of grasps filtered by cutting plane = " << num_grasps_filtered);
}


std::size_t GraspFilter::filterGraspsHelper(std::vector<GraspCandidatePtr>& grasp_candidates,
                                            planning_scene_monitor::PlanningSceneMonitorPtr planning_scene_monitor,
                                            const robot_model::JointModelGroup* arm_jmg,
                                            bool filter_pregrasp,
                                            bool verbose, bool collision_verbose)
{
  // -----------------------------------------------------------------------------------------------
  // Setup collision checking

  // Copy planning scene that is locked
  planning_scene::PlanningScenePtr cloned_scene;
  {
    planning_scene_monitor::LockedPlanningSceneRO scene(planning_scene_monitor);
    cloned_scene = planning_scene::PlanningScene::clone(scene);
  }
  *robot_state_ = cloned_scene->getCurrentState();

  // -----------------------------------------------------------------------------------------------
  // Choose Number of cores
  std::size_t num_threads = omp_get_max_threads();
  if( num_threads > grasp_candidates.size() )
    num_threads = grasp_candidates.size();

  // Debug
  if(verbose)
  {
    num_threads = 1;
    ROS_WARN_STREAM_NAMED("grasp_filter","Using only " << num_threads << " threads");
  }
  ROS_INFO_STREAM_NAMED("filter", "Filtering possible grasps with " << num_threads << " threads");

  // -----------------------------------------------------------------------------------------------
  // Load kinematic solvers if not already loaded
  if( kin_solvers_[arm_jmg->getName()].size() != num_threads )
  {
    kin_solvers_[arm_jmg->getName()].clear();

    // Create an ik solver for every thread
    for (int i = 0; i < num_threads; ++i)
    {
      //ROS_DEBUG_STREAM_NAMED("filter","Creating ik solver " << i);
      kin_solvers_[arm_jmg->getName()].push_back(arm_jmg->getSolverInstance());

      // Test to make sure we have a valid kinematics solver
      if( !kin_solvers_[arm_jmg->getName()][i] )
      {
        ROS_ERROR_STREAM_NAMED("grasp_filter","No kinematic solver found");
        return 0;
      }
    }
  }

  // Robot states -------------------------------------------------------------------------------
  // Create an robot state for every thread
  if( robot_states_.size() != num_threads )
  {
    robot_states_.clear();
    for (int i = 0; i < num_threads; ++i)
    {
      // Copy the previous robot state
      robot_states_.push_back(moveit::core::RobotStatePtr(new moveit::core::RobotState(*robot_state_)));
    }
  }
  else // update the states
  {
    for (int i = 0; i < num_threads; ++i)
    {
      // Copy the previous robot state
      *(robot_states_[i]) = *robot_state_;
    }
  }

  // Transform poses -------------------------------------------------------------------------------
  // bring the pose to the frame of the IK solver
  const std::string &ik_frame = kin_solvers_[arm_jmg->getName()][0]->getBaseFrame();
  Eigen::Affine3d link_transform;
  ROS_DEBUG_STREAM_NAMED("grasp_filter","Frame transform from ik_frame: " << ik_frame << " and robot model frame: " << robot_state_->getRobotModel()->getModelFrame());
  if (!moveit::core::Transforms::sameFrame(ik_frame, robot_state_->getRobotModel()->getModelFrame()))
  {
    const robot_model::LinkModel *lm = robot_state_->getLinkModel((!ik_frame.empty() && ik_frame[0] == '/') ? ik_frame.substr(1) : ik_frame);

    if (!lm)
    {
      ROS_ERROR_STREAM_NAMED("grasp_filter","Unable to find frame for link transform");
      return 0;
    }

    link_transform = robot_state_->getGlobalLinkTransform(lm).inverse();
  }

  // Thread data -------------------------------------------------------------------------------
  // Allocate only once to increase performance
  std::vector<IkThreadStructPtr> ik_thread_structs;
  ik_thread_structs.resize(num_threads);
  for (int thread_id = 0; thread_id < num_threads; ++thread_id)
  {
    ik_thread_structs[thread_id].reset(new moveit_grasps::IkThreadStruct(grasp_candidates,
                                                                         cloned_scene,
                                                                         link_transform,
                                                                         0, // this is filled in by OpenMP
                                                                         kin_solvers_[arm_jmg->getName()][thread_id],
                                                                         robot_states_[thread_id],
                                                                         solver_timeout_,
                                                                         filter_pregrasp,
                                                                         verbose,
                                                                         collision_verbose,
                                                                         thread_id));
    ik_thread_structs[thread_id]->ik_seed_state_.resize(num_variables_); // fill with zeros TODO- give a seed state!;
  }

  // Benchmark time
  ros::Time start_time;
  start_time = ros::Time::now();

  // -----------------------------------------------------------------------------------------------
  // Loop through poses and find those that are kinematically feasible

  omp_set_num_threads(num_threads);
#pragma omp parallel for schedule(dynamic)
  for (std::size_t grasp_id = 0; grasp_id < grasp_candidates.size(); ++grasp_id)
  {
    std::size_t thread_id = omp_get_thread_num();
    ROS_DEBUG_STREAM_NAMED("filter.superdebug","Thread " << thread_id << " processing grasp " << grasp_id);

    // Assign grasp to process
    ik_thread_structs[thread_id]->grasp_id = grasp_id;

    // Process the grasp
    processCandidateGrasp(ik_thread_structs[thread_id]);
  }

  // Count number of grasps remaining
  std::size_t remaining_grasps = 0;
  std::size_t grasp_filtered_by_ik = 0;
  std::size_t grasp_filtered_by_collision = 0;
  std::size_t grasp_filtered_by_obstruction = 0;
  std::size_t grasp_filtered_by_orientation = 0;
  std::size_t pregrasp_filtered_by_ik = 0;
  std::size_t pregrasp_filtered_by_collision = 0;

  for (std::size_t i = 0; i < grasp_candidates.size(); ++i)
  {
    if (grasp_candidates[i]->grasp_filtered_by_ik_)
      grasp_filtered_by_ik++;
    if (grasp_candidates[i]->grasp_filtered_by_collision_)
      grasp_filtered_by_collision++;
    if (grasp_candidates[i]->grasp_filtered_by_obstruction_)
      grasp_filtered_by_obstruction++;
    if (grasp_candidates[i]->grasp_filtered_by_orientation_)
      grasp_filtered_by_orientation++;
    if (grasp_candidates[i]->pregrasp_filtered_by_ik_)
      pregrasp_filtered_by_ik++;
    if (grasp_candidates[i]->pregrasp_filtered_by_collision_)
      pregrasp_filtered_by_collision++;
    if (grasp_candidates[i]->valid_)
      remaining_grasps++;
  }

  if (remaining_grasps +
      grasp_filtered_by_ik +
      grasp_filtered_by_collision +
      grasp_filtered_by_obstruction + 
      grasp_filtered_by_orientation +
      pregrasp_filtered_by_ik +
      pregrasp_filtered_by_collision != grasp_candidates.size())
    ROS_ERROR_STREAM_NAMED("filter","Logged filter reasons do not add up to total number of grasps. Internal error.");


  std::cout << "-------------------------------------------------------" << std::endl;
  std::cout << "GRASPING RESULTS " << std::endl;
  std::cout << "total candidate grasps         " << grasp_candidates.size() << std::endl;
  std::cout << "grasp_filtered_by_ik           " << grasp_filtered_by_ik << std::endl;
  std::cout << "grasp_filtered_by_collision    " << grasp_filtered_by_collision << std::endl;
  std::cout << "grasp_filtered_by_obstruction  " << grasp_filtered_by_obstruction << std::endl;
  std::cout << "grasp_filtered_by_orientation  " << grasp_filtered_by_orientation << std::endl;
  std::cout << "pregrasp_filtered_by_ik        " << pregrasp_filtered_by_ik << std::endl;
  std::cout << "pregrasp_filtered_by_collision " << pregrasp_filtered_by_collision << std::endl;
  std::cout << "remaining grasps               " << remaining_grasps << std::endl;
  std::cout << "-------------------------------------------------------" << std::endl;

  if (false)
  {
    // End Benchmark time
    double duration = (ros::Time::now() - start_time).toNSec() * 1e-6;
    ROS_INFO_STREAM_NAMED("filter","Grasp generator IK grasp filtering benchmark time:");
    std::cout << duration << "\t" << grasp_candidates.size() << "\n";
  }

  return remaining_grasps;
}

bool GraspFilter::processCandidateGrasp(IkThreadStructPtr& ik_thread_struct)
{
  ROS_DEBUG_STREAM_NAMED("filter.superdebug", "Checking grasp #" << ik_thread_struct->grasp_id);

  // Helper pointer
  GraspCandidatePtr& grasp_candidate = ik_thread_struct->grasp_candidates_[ik_thread_struct->grasp_id];

  // Get pose
  ik_thread_struct->ik_pose_ = grasp_candidate->grasp_.grasp_pose;

  // Debug
  if (ik_thread_struct->verbose_ && false)
  {
    ik_thread_struct->ik_pose_.header.frame_id = ik_thread_struct->kin_solver_->getBaseFrame();
    visual_tools_->publishZArrow(ik_thread_struct->ik_pose_.pose, rviz_visual_tools::RED, rviz_visual_tools::REGULAR, 0.1);
  }

  moveit::core::GroupStateValidityCallbackFn constraint_fn
    = boost::bind(&isGraspStateValid, ik_thread_struct->planning_scene_.get(),
                  ik_thread_struct->collision_verbose_, collision_verbose_speed_, visual_tools_, _1, _2, _3);

    // skip grasps that have already been rejected by obstructions and orientation filters
    if (grasp_candidate->grasp_filtered_by_obstruction_ || grasp_candidate->grasp_filtered_by_orientation_)
      continue;

    bool collision_checking_verbose = false;
    moveit::core::GroupStateValidityCallbackFn constraint_fn
      = boost::bind(&isGraspStateValid, ik_thread_struct.planning_scene_.get(),
                    collision_checking_verbose, visual_tools_, _1, _2, _3);

  // Set gripper position (how open the fingers are) to OPEN
  grasp_candidate->grasp_data_->setRobotStatePreGrasp(ik_thread_struct->robot_state_);

  // Solve IK Problem
  if (!findIKSolution(grasp_candidate->grasp_ik_solution_, 
                      ik_thread_struct, grasp_candidate, constraint_fn))
  {
    ROS_DEBUG_STREAM_NAMED("filter.superdebug","the-grasp: unable to find IK solution");
    grasp_candidate->grasp_filtered_by_ik_ = true;
    return false;
  }

  // Copy solution to seed state so that next solution is faster
  ik_thread_struct->ik_seed_state_ = grasp_candidate->grasp_ik_solution_;

  // Check for collision
  if (secondary_collision_checking_)
    if (checkInCollision(grasp_candidate->grasp_ik_solution_, ik_thread_struct, ik_thread_struct->collision_verbose_))
    {
      ROS_DEBUG_STREAM_NAMED("filter.superdebug","the-grasp: arm position is in collision");
      grasp_candidate->grasp_filtered_by_collision_ = true;
      return false;
    }

  // Start pre-grasp section ----------------------------------------------------------
  if (ik_thread_struct->filter_pregrasp_)       // optionally check the pregrasp
  {
    // Convert to a pre-grasp
    const std::string &ee_parent_link_name = grasp_candidate->grasp_data_->ee_jmg_->getEndEffectorParentGroup().second;
    ik_thread_struct->ik_pose_ = GraspGenerator::getPreGraspPose(grasp_candidate->grasp_, ee_parent_link_name);

    // Set gripper position (how open the fingers are) to CLOSED
    grasp_candidate->grasp_data_->setRobotStateGrasp(ik_thread_struct->robot_state_);

    // Solve IK Problem
    if (!findIKSolution(grasp_candidate->pregrasp_ik_solution_, ik_thread_struct, grasp_candidate, constraint_fn))
    {
      ROS_DEBUG_STREAM_NAMED("filter.superdebug","pre-grasp: unable to find IK solution");
      grasp_candidate->pregrasp_filtered_by_ik_ = true;
      return false;
    }

    // Check for collision
    if (secondary_collision_checking_)
      if (checkInCollision(grasp_candidate->pregrasp_ik_solution_, ik_thread_struct, ik_thread_struct->collision_verbose_))
      {
        ROS_DEBUG_STREAM_NAMED("filter.superdebug","pre-grasp: arm position is in collision");
        grasp_candidate->pregrasp_filtered_by_collision_ = true;
        return false;
      }
  }

  // Grasp is valid
  grasp_candidate->valid_ = true;

  return true;
}

bool GraspFilter::findIKSolution(std::vector<double>& ik_solution, IkThreadStructPtr& ik_thread_struct, GraspCandidatePtr& grasp_candidate,                                 
                                 const moveit::core::GroupStateValidityCallbackFn &constraint_fn)
{
  // Transform current pose to frame of planning group
  Eigen::Affine3d eigen_pose;
  tf::poseMsgToEigen(ik_thread_struct->ik_pose_.pose, eigen_pose);
  eigen_pose = ik_thread_struct->link_transform_ * eigen_pose;
  tf::poseEigenToMsg(eigen_pose, ik_thread_struct->ik_pose_.pose);

  // Clear out previous solution just in case - not sure if this is needed
  ik_solution.clear(); // TODO remove

  // Set callback function
  kinematics::KinematicsBase::IKCallbackFn ik_callback_fn;
  if (constraint_fn)
    ik_callback_fn = boost::bind(&ikCallbackFnAdapter, ik_thread_struct->robot_state_.get(),
                                 grasp_candidate->grasp_data_->arm_jmg_, constraint_fn, _1, _2, _3);

  // Test it with IK
  ik_thread_struct->kin_solver_->searchPositionIK(ik_thread_struct->ik_pose_.pose, ik_thread_struct->ik_seed_state_, ik_thread_struct->timeout_, ik_solution,
                                                  ik_callback_fn, ik_thread_struct->error_code_);

  // Results
  if( ik_thread_struct->error_code_.val == moveit_msgs::MoveItErrorCodes::NO_IK_SOLUTION )
  {
    // The grasp was valid but the pre-grasp was not
    ROS_DEBUG_STREAM_NAMED("filter.superdebug","No IK solution");
    return false;
  }
  else if( ik_thread_struct->error_code_.val == moveit_msgs::MoveItErrorCodes::TIMED_OUT )
  {
    ROS_DEBUG_STREAM_NAMED("filter.superdebug","Timed Out.");
    return false;
  }
  else if( ik_thread_struct->error_code_.val != moveit_msgs::MoveItErrorCodes::SUCCESS )
  {
    ROS_ERROR_STREAM_NAMED("filter","Unknown MoveItErrorCode from IK solver: " << ik_thread_struct->error_code_.val);
    return false;
  }

  return true;
}

bool GraspFilter::checkInCollision(std::vector<double>& ik_solution, IkThreadStructPtr& ik_thread_struct, bool verbose)
{
  // Apply joint values to robot state
  ik_thread_struct->robot_state_->setJointGroupPositions(ik_thread_struct->arm_jmg_, ik_solution);

  if (ik_thread_struct->planning_scene_->isStateColliding(*ik_thread_struct->robot_state_, ik_thread_struct->arm_jmg_->getName(), verbose))
  {
    if (verbose)
    {
      ROS_ERROR_STREAM_NAMED("filter","Grasp solution colliding");
      visual_tools_->publishRobotState(ik_thread_struct->robot_state_, rviz_visual_tools::RED);
      visual_tools_->publishContactPoints(*ik_thread_struct->robot_state_, ik_thread_struct->planning_scene_.get(), rviz_visual_tools::BLACK);
      ros::Duration(collision_verbose_speed_).sleep();
    }
    return true;
  }

  // Debug valid state
  if (verbose)
  {
    ROS_INFO_STREAM_NAMED("filter","Valid grasp solution");
    visual_tools_->publishRobotState(ik_thread_struct->robot_state_, rviz_visual_tools::GREEN);
    ros::Duration(collision_verbose_speed_).sleep();
  }

  return false;
}

bool GraspFilter::chooseBestGrasp( std::vector<GraspCandidatePtr>& grasp_candidates,
                                   GraspCandidatePtr& chosen )
{
  // Find max grasp quality
  double max_quality = -1;
  bool found_valid_grasp = false;
  for (std::size_t i = 0; i < grasp_candidates.size(); ++i)
  {
    // Check if valid grasp
    if (!grasp_candidates[i]->valid_)
    {
      continue; // not valid
    }

    // METHOD 1 - use score
    if (true)
    {
      if (grasp_candidates[i]->grasp_.grasp_quality > max_quality)
      {
        max_quality = grasp_candidates[i]->grasp_.grasp_quality;
        chosen = grasp_candidates[i];
      }
    }
    else // METHOD 2 - use yall angle
    {
      const geometry_msgs::Pose& pose = grasp_candidates[i]->grasp_.grasp_pose.pose;
      //double roll = atan2(2*(pose.orientation.x*pose.orientation.y + pose.orientation.w*pose.orientation.z), pose.orientation.w*pose.orientation.w + pose.orientation.x*pose.orientation.x - pose.orientation.y*pose.orientation.y - pose.orientation.z*pose.orientation.z);
      double yall = asin(-2*(pose.orientation.x*pose.orientation.z - pose.orientation.w*pose.orientation.y));
      //double pitch = atan2(2*(pose.orientation.y*pose.orientation.z + pose.orientation.w*pose.orientation.x), pose.orientation.w*pose.orientation.w - pose.orientation.x*pose.orientation.x - pose.orientation.y*pose.orientation.y + pose.orientation.z*pose.orientation.z);
      //std::cout << "ROLL: " << roll << " YALL: " << yall << " PITCH: " << pitch << std::endl;
      std::cout << "YALL: " << yall << std::endl;
      if (yall > max_quality)
      {
        max_quality = yall;
        chosen = grasp_candidates[i];
      }
    }

  }

  ROS_INFO_STREAM_NAMED("grasp_filter","Chose grasp with quality " << max_quality);

  return true;
}

bool GraspFilter::visualizeGrasps(const std::vector<GraspCandidatePtr>& grasp_candidates,
                                  const moveit::core::JointModelGroup *arm_jmg)
{
  // Publish in batch
  visual_tools_->enableBatchPublishing(true);

  /*
    RED - grasp filtered by ik
    PINK - grasp filtered by collision
    MAGENTA - grasp filtered by obstruction
    YELLOW - grasp filtered by orientation
    BLUE - pregrasp filtered by ik
    CYAN - pregrasp filtered by collision
    GREEN - valid
  */

  for (std::size_t i = 0; i < grasp_candidates.size(); ++i)
  {
    if (grasp_candidates[i]->grasp_filtered_by_ik_)
    {
      visual_tools_->publishZArrow(grasp_candidates[i]->grasp_.grasp_pose.pose, rviz_visual_tools::RED);
    }
    else if (grasp_candidates[i]->grasp_filtered_by_collision_)
    {
      visual_tools_->publishZArrow(grasp_candidates[i]->grasp_.grasp_pose.pose, rviz_visual_tools::PINK);
    }
    else if (grasp_candidates[i]->pregrasp_filtered_by_ik_)
    {
      visual_tools_->publishZArrow(grasp_candidates[i]->grasp_.grasp_pose.pose, rviz_visual_tools::BLUE);
    }
    else if (grasp_candidates[i]->grasp_filtered_by_obstruction_)
    {
      visual_tools_->publishZArrow(grasp_candidates[i]->grasp_.grasp_pose.pose, rviz_visual_tools::MAGENTA);
    }
    else if (grasp_candidates[i]->grasp_filtered_by_orientation_)
    {
      visual_tools_->publishZArrow(grasp_candidates[i]->grasp_.grasp_pose.pose, rviz_visual_tools::YELLOW);
    }
    else if (grasp_candidates[i]->pregrasp_filtered_by_collision_)
    {
      visual_tools_->publishZArrow(grasp_candidates[i]->grasp_.grasp_pose.pose, rviz_visual_tools::CYAN);
    }
    else
      visual_tools_->publishZArrow(grasp_candidates[i]->grasp_.grasp_pose.pose, rviz_visual_tools::GREEN);
  }

  // Publish in batch
  visual_tools_->triggerBatchPublishAndDisable();

  return true;
}

bool GraspFilter::visualizeIKSolutions(const std::vector<GraspCandidatePtr>& grasp_candidates,
                                       const moveit::core::JointModelGroup* arm_jmg, double animation_speed)
{
  // Convert the grasp_candidates into a format moveit_visual_tools can use
  std::vector<trajectory_msgs::JointTrajectoryPoint> ik_solutions;
  for (std::size_t i = 0; i < grasp_candidates.size(); ++i)
  {
    // Check if valid grasp
    if (!grasp_candidates[i]->valid_)
      continue;

    trajectory_msgs::JointTrajectoryPoint new_point;
    new_point.positions = grasp_candidates[i]->grasp_ik_solution_;
    ik_solutions.push_back(new_point);
  }

  if (ik_solutions.empty())
  {
    ROS_ERROR_STREAM_NAMED("filter","Unable to visualize IK solutions because there are no valid ones");
    return false;
  }

  return visual_tools_->publishIKSolutions(ik_solutions, arm_jmg, animation_speed);
}

bool GraspFilter::visualizeCandidateGrasps(const std::vector<GraspCandidatePtr>& grasp_candidates)
{
  for (std::size_t i = 0; i < grasp_candidates.size(); ++i)
  {
    // Check if valid grasp
    if (!grasp_candidates[i]->valid_)
      continue;

    // Apply the pregrasp state
    grasp_candidates[i]->getPreGraspState(robot_state_);

    // Show in Rviz
    visual_tools_->publishRobotState(robot_state_);
    ros::Duration(show_filtered_arm_solutions_pregrasp_speed_).sleep();

    // Apply the grasp state
    grasp_candidates[i]->getGraspState(robot_state_);

    // Show in Rviz
    visual_tools_->publishRobotState(robot_state_);
    ros::Duration(show_filtered_arm_solutions_speed_).sleep();
  }

  return true;
}

} // namespace

namespace
{
bool isGraspStateValid(const planning_scene::PlanningScene *planning_scene, bool verbose, double verbose_speed,
                       moveit_visual_tools::MoveItVisualToolsPtr visual_tools, robot_state::RobotState *robot_state,
                       const robot_state::JointModelGroup *group, const double *ik_solution)
{
  robot_state->setJointGroupPositions(group, ik_solution);
  robot_state->update();

  if (!planning_scene)
  {
    ROS_ERROR_STREAM_NAMED("manipulation","No planning scene provided");
    return false;
  }
  if (!planning_scene->isStateColliding(*robot_state, group->getName()))
    return true; // not in collision

  // Display more info about the collision
  if (verbose)
  {
    visual_tools->publishRobotState(*robot_state, rviz_visual_tools::RED);
    planning_scene->isStateColliding(*robot_state, group->getName(), true);
    visual_tools->publishContactPoints(*robot_state, planning_scene);
    ros::Duration(verbose_speed).sleep();
  }
  return false;
}
}
