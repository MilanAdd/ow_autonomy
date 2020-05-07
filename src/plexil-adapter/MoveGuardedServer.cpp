// Temporary file.  An experimental substitute action server for MoveGuarded,
// which really belongs in the simulator.

#include <ros/ros.h>
#include <actionlib/server/simple_action_server.h>
#include <ow_autonomy/MoveGuardedAction.h>
#include <string>

using std::string;

class MoveGuardedAction
{
 protected:

  ros::NodeHandle m_nodeHandle;
  actionlib::SimpleActionServer<ow_autonomy::MoveGuardedAction> m_actionServer;
  string m_actionName;
  ow_autonomy::MoveGuardedFeedback m_feedback;
  ow_autonomy::MoveGuardedResult m_result;

 public:

  MoveGuardedAction (string name)
    : m_actionServer(m_nodeHandle, name,
                     boost::bind(&MoveGuardedAction::executeCB, this, _1), false),
      m_actionName(name)
  {
    m_actionServer.start();
  }

  ~MoveGuardedAction () { }

  void executeCB (const ow_autonomy::MoveGuardedGoalConstPtr& goal)
  {
    // helper variables
    ros::Rate r(1);
    bool success = true;

    m_feedback.current_x = m_feedback.current_y = m_feedback.current_z = 0;
    ROS_INFO ("%s: Executing", m_actionName.c_str());

    // start executing the action
    for (int i = 1; i <= 100; i++) {
      if (m_actionServer.isPreemptRequested() || !ros::ok()) {
        ROS_INFO ("%s: Preempted", m_actionName.c_str());
        // set the action state to preempted
        m_actionServer.setPreempted();
        success = false;
        break;
      }
      m_feedback.current_x = i;
      m_actionServer.publishFeedback (m_feedback);
      r.sleep();
    }

    if (success) {
      m_result.message = "Move Guarded Action succeeded!";
      ROS_INFO ("%s: Succeeded", m_actionName.c_str());
      m_actionServer.setSucceeded (m_result);
    }
  }
};


int main (int argc, char** argv)
{
  ros::init(argc, argv, "MoveGuarded");

  MoveGuardedAction MoveGuarded("MoveGuarded");
  ros::spin();

  return 0;
}
