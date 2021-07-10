// The Notices and Disclaimers for Ocean Worlds Autonomy Testbed for Exploration
// Research and Simulation can be found in README.md in the root directory of
// this repository.

// ROS
#include <ros/ros.h>
#include <ros/package.h>
//Plan selection 
#include "PlexilPlanSelection.h"

int main(int argc, char* argv[])
{
  // Initializations
  ros::init(argc, argv, "plexil_plan_selection_node");
  std::string initial_plan = "None";
  std::string plan(argv[1]);
  //checking if there is a plan given
  if(argc == 2 && plan.compare("None") != 0) {
    initial_plan = plan;
  }
  PlexilPlanSelection plan_selection; 
  plan_selection.initialize(initial_plan); //initialize pubs, subs, etc
  plan_selection.start(); //begin control loop
  ros::spin();
  return 0;  // We never actually get here!
}
