// Copyright (c) 2018-2020, United States Government as represented by the
// Administrator of the National Aeronautics and Space Administration. All
// rights reserved.

// ow_autonomy
#include "OwInterface.h"
#include "subscriber.h"
#include "joint_support.h"

// OW - other
#include <ow_lander/StartPlanning.h>
#include <ow_lander/MoveGuarded.h>
#include <ow_lander/PublishTrajectory.h>

// ROS
#include <std_msgs/Float64.h>
#include <std_msgs/Empty.h>
#include <sensor_msgs/Image.h>

// C++
#include <thread>
#include <set>
#include <map>
using std::set;
using std::map;

// C
#include <cmath>  // for M_PI and abs

// Degree/Radian
const double D2R = M_PI / 180.0 ;
const double R2D = 180.0 / M_PI ;

// Static initialization
OwInterface* OwInterface::m_instance = nullptr;


////////////////////////////// Service Support ///////////////////////////////////

// Service names/types as used in both PLEXIL and ow_lander
const string MoveGuardedService = "MoveGuarded";
const string ArmPlanningService = "StartPlanning";
const string ArmTrajectoryService = "PublishTrajectory";

template<class Service>
class ServiceInfo
{
  // Static class that manages, for each service type, its running state.
 public:
  static bool is_running () { return m_running; }
  static void start () { m_running = true; }
  static void stop () { m_running = false; }

 private:
  static bool m_running;
};

// Static initialization for class above.
template<class Service>
bool ServiceInfo<Service>::m_running = false;

// Map from each service name to its running check function.
typedef bool (*boolfn)();
const map<string, boolfn> ServiceRunning {
  {MoveGuardedService, ServiceInfo<ow_lander::MoveGuarded>::is_running},
  {ArmPlanningService, ServiceInfo<ow_lander::StartPlanning>::is_running},
  {ArmTrajectoryService, ServiceInfo<ow_lander::PublishTrajectory>::is_running}
};

static bool is_service (const string name)
{
  return ServiceRunning.find (name) != ServiceRunning.end();
}

template<class Service>
static void service_call (ros::ServiceClient client, Service srv, string name)
{
  // NOTE: arguments are copies because this function is called in a thread that
  // outlives its caller.  Assumption checked upstream: service is available
  // (not already running).

  if (ServiceInfo<Service>::is_running()) {
    ROS_ERROR("service_call: %s in running state. This shouldn't happen.",
              name.c_str());
  }
  else ServiceInfo<Service>::start();
  publish ("Running", true, name);

  if (client.call (srv)) { // blocks
    ROS_INFO("%s returned: %d, %s", name.c_str(), srv.response.success,
             srv.response.message.c_str());  // make DEBUG later
  }
  else ROS_ERROR ("Failed to call service %s", name.c_str());

  if (! ServiceInfo<Service>::is_running()) {
    ROS_ERROR("service_call: %s in stopped state. This shouldn't happen.",
              name.c_str());
  }
  else ServiceInfo<Service>::stop();
  publish ("Finished", true, name);
}


static bool service_client_ok (ros::ServiceClient& client)
{
  if (! client.exists()) {
    ROS_ERROR("Service client does not exist!");
    return false;
  }

  if (! client.isValid()) {
    ROS_ERROR("Service client is invalid!");
    return false;
  }

  return true;
}

template<class Service>
static bool service_available (const string& name)
{
  if (ServiceInfo<Service>::is_running()) {
    ROS_WARN("Service %s already running, ignoring request.", name.c_str());
    return false;
  }

  return true;
}


/////////////////////////// Joint/Torque Support ///////////////////////////////

static set<string> JointsAtHardTorqueLimit { };
static set<string> JointsAtSoftTorqueLimit { };

static map<string, Joint> JointMap {
  // ROS JointStates message name -> type
  { "j_shou_yaw", Joint::shoulder_yaw },
  { "j_shou_pitch", Joint::shoulder_pitch },
  { "j_prox_pitch", Joint::proximal_pitch },
  { "j_dist_pitch", Joint::distal_pitch },
  { "j_hand_yaw", Joint::hand_yaw },
  { "j_scoop_yaw", Joint::scoop_yaw },
  { "j_ant_pan", Joint::antenna_pan },
  { "j_ant_tilt", Joint::antenna_tilt }
};

static map<Joint, JointProperties> JointPropMap {
  // NOTE: Torque limits are made up, and there may be a better place for these
  // later.  Assuming that only magnitude matters.

  { Joint::shoulder_yaw,   { "j_shou_yaw", "ShoulderYaw", 60, 80 }},
  { Joint::shoulder_pitch, { "j_shou_pitch", "ShoulderPitch", 60, 80 }},
  { Joint::proximal_pitch, { "j_prox_pitch", "ProximalPitch", 60, 80 }},
  { Joint::distal_pitch,   { "j_dist_pitch", "DistalPitch", 60, 80 }},
  { Joint::hand_yaw,       { "j_hand_yaw", "HandYaw", 60, 80 }},
  { Joint::scoop_yaw,      { "j_scoop_yaw", "ScoopYaw", 60, 80 }},
  { Joint::antenna_pan,    { "j_ant_pan", "AntennaPan", 30, 30 }},
  { Joint::antenna_tilt,   { "j_ant_tilt", "AntennaTilt", 30, 30 }}
};

static map<Joint, JointTelemetry> JointTelemetryMap { };

static void handle_overtorque (Joint joint, double effort)
{
  // For now, torque is just effort (Newton-meter), and overtorque is specific
  // to the joint.

  if (abs(effort) >= JointPropMap[joint].hardTorqueLimit) {
    JointsAtHardTorqueLimit.insert (JointPropMap[joint].plexilName);
  }
  else if (abs(effort) >= JointPropMap[joint].softTorqueLimit) {
    JointsAtSoftTorqueLimit.insert(JointPropMap[joint].plexilName);
  }
  else {
    JointsAtHardTorqueLimit.erase (JointPropMap[joint].plexilName);
    JointsAtSoftTorqueLimit.erase (JointPropMap[joint].plexilName);
  }
}

static void handle_joint_fault (Joint joint, int joint_index,
                                const sensor_msgs::JointState::ConstPtr& msg)
{
  // NOTE: For now, the only fault is overtorque.
  handle_overtorque (joint, msg->effort[joint_index]);
}

void OwInterface::jointStatesCallback
(const sensor_msgs::JointState::ConstPtr& msg)
{
  // Publish all joint information for visibility to PLEXIL and handle any
  // joint-related faults.

  for (int i = 0; i < JointMap.size(); i++) {
    string ros_name = msg->name[i];
    if (JointMap.find (ros_name) != JointMap.end()) {
      Joint joint = JointMap[ros_name];
      JointTelemetryMap[joint] = JointTelemetry (msg->position[i],
                                                 msg->velocity[i],
                                                 msg->effort[i]);
      string plexil_name = JointPropMap[joint].plexilName;
      publish (plexil_name + "Velocity", msg->velocity[i]);
      publish (plexil_name + "Effort", msg->effort[i]);
      publish (plexil_name + "Position", msg->position[i]);
      handle_joint_fault (joint, i, msg);
    }
    else ROS_ERROR("jointStatesCallback: unsupported joint %s",
                   ros_name.c_str());
  }
}


////////////////////////////// Image Support ///////////////////////////////////

static double CurrentTilt         = 0.0;
static double CurrentPanDegrees   = 0.0;
static bool   ImageReceived       = false;

static void pan_callback
(const control_msgs::JointControllerState::ConstPtr& msg)
{
  CurrentPanDegrees = msg->set_point * R2D;
  publish ("PanDegrees", CurrentPanDegrees);
}

static void tilt_callback
(const control_msgs::JointControllerState::ConstPtr& msg)
{
  CurrentTilt = msg->set_point * R2D;
  publish ("TiltDegrees", CurrentTilt);
}

static void camera_callback (const sensor_msgs::Image::ConstPtr& msg)
{
  // Assuming that receipt of this message is success itself.
  ImageReceived = true;
  publish ("ImageReceived", ImageReceived);
}


/////////////////////////// OwInterface members ////////////////////////////////

OwInterface* OwInterface::instance ()
{
  // Very simple singleton
  if (m_instance == nullptr) m_instance = new OwInterface();
  return m_instance;
}

OwInterface::OwInterface ()
  : m_genericNodeHandle (nullptr),
    m_antennaTiltPublisher (nullptr),
    m_antennaPanPublisher (nullptr),
    m_leftImageTriggerPublisher (nullptr),
    m_antennaTiltSubscriber (nullptr),
    m_antennaPanSubscriber (nullptr),
    m_jointStatesSubscriber (nullptr),
    m_cameraSubscriber (nullptr)
{ }

OwInterface::~OwInterface ()
{
  if (m_genericNodeHandle) delete m_genericNodeHandle;
  if (m_antennaTiltPublisher) delete m_antennaTiltPublisher;
  if (m_leftImageTriggerPublisher) delete m_leftImageTriggerPublisher;
  if (m_antennaTiltSubscriber) delete m_antennaTiltSubscriber;
  if (m_antennaPanSubscriber) delete m_antennaPanSubscriber;
  if (m_jointStatesSubscriber) delete m_jointStatesSubscriber;
  if (m_cameraSubscriber) delete m_cameraSubscriber;
  if (m_instance) delete m_instance;
}

void OwInterface::initialize()
{
  static bool initialized = false;

  if (not initialized) {
    m_genericNodeHandle = new ros::NodeHandle();

    // Initialize publishers.  Queue size is a guess at adequacy.  For now,
    // latching in lieu of waiting for publishers.

    const int qsize = 3;
    const bool latch = true;
    m_antennaTiltPublisher = new ros::Publisher
      (m_genericNodeHandle->advertise<std_msgs::Float64>
       ("/ant_tilt_position_controller/command", qsize, latch));
    m_antennaPanPublisher = new ros::Publisher
      (m_genericNodeHandle->advertise<std_msgs::Float64>
       ("/ant_pan_position_controller/command", qsize, latch));
    m_leftImageTriggerPublisher = new ros::Publisher
      (m_genericNodeHandle->advertise<std_msgs::Empty>
       ("/StereoCamera/left/image_trigger", qsize, latch));

    // Initialize subscribers

    m_antennaTiltSubscriber = new ros::Subscriber
      (m_genericNodeHandle ->
       subscribe("/ant_tilt_position_controller/state", qsize, tilt_callback));
    m_antennaPanSubscriber = new ros::Subscriber
      (m_genericNodeHandle ->
       subscribe("/ant_pan_position_controller/state", qsize, pan_callback));
    m_jointStatesSubscriber = new ros::Subscriber
      (m_genericNodeHandle ->
       subscribe("/joint_states", qsize, OwInterface::jointStatesCallback));
    m_cameraSubscriber = new ros::Subscriber
      (m_genericNodeHandle ->
       subscribe("/StereoCamera/left/image_raw", qsize, camera_callback));
  }
}

void OwInterface::startPlanningDemo()
{
  if (! service_available<ow_lander::StartPlanning>(ArmPlanningService)) return;

  ros::NodeHandle nhandle ("planning");

  ros::ServiceClient client =
    // NOTE: typo is deliberate
    nhandle.serviceClient<ow_lander::StartPlanning>("start_plannning_session");

  if (service_client_ok (client)) {
    ow_lander::StartPlanning srv;
    srv.request.use_defaults = true;
    srv.request.trench_x = 0.0;
    srv.request.trench_y = 0.0;
    srv.request.trench_d = 0.0;
    srv.request.delete_prev_traj = false;
    std::thread t (service_call<ow_lander::StartPlanning>,
                   client, srv, ArmPlanningService);
    t.detach();
  }
}

void OwInterface::moveGuardedDemo()
{
  moveGuarded();
}

void OwInterface::moveGuarded (double target_x, double target_y, double target_z,
                               double surf_norm_x,
                               double surf_norm_y,
                               double surf_norm_z,
                               double offset_dist, double overdrive_dist,
                               bool delete_prev_traj,
                               bool retract)
{
  if (! service_available<ow_lander::StartPlanning>(MoveGuardedService)) return;

  ros::NodeHandle nhandle ("planning");

  ros::ServiceClient client =
    nhandle.serviceClient<ow_lander::MoveGuarded>("start_move_guarded");

  if (service_client_ok (client)) {
    ow_lander::MoveGuarded srv;
    srv.request.use_defaults = false;
    srv.request.target_x = target_x;
    srv.request.target_y = target_y;
    srv.request.target_z = target_z;
    srv.request.surface_normal_x = surf_norm_x;
    srv.request.surface_normal_y = surf_norm_y;
    srv.request.surface_normal_z = surf_norm_z;
    srv.request.offset_distance = offset_dist;
    srv.request.overdrive_distance = overdrive_dist;
    srv.request.retract = retract;
    std::thread t (service_call<ow_lander::MoveGuarded>,
                   client, srv, MoveGuardedService);
    t.detach();
  }
}

void OwInterface::publishTrajectoryDemo()
{
  if (! service_available<ow_lander::StartPlanning>(ArmTrajectoryService)) return;

  ros::NodeHandle nhandle ("planning");

  ros::ServiceClient client =
    nhandle.serviceClient<ow_lander::PublishTrajectory>("publish_trajectory");

  if (service_client_ok (client)) {
    ow_lander::PublishTrajectory srv;
    srv.request.use_latest = true;
    srv.request.trajectory_filename = "ow_lander_trajectory.txt";
    std::thread t (service_call<ow_lander::PublishTrajectory>,
                   client, srv, ArmTrajectoryService);
    t.detach();
  }
}

void OwInterface::tiltAntenna (double arg)
{
  std_msgs::Float64 msg;
  msg.data = arg * D2R;
  ROS_INFO("Tilting to %f degrees (%f radians)", arg, msg.data);
  m_antennaTiltPublisher->publish (msg);
}

void OwInterface::panAntenna (double arg)
{
  std_msgs::Float64 msg;
  msg.data = arg * D2R;
  ROS_INFO("Panning to %f degrees (%f radians)", arg, msg.data);
  m_antennaPanPublisher->publish (msg);
}

void OwInterface::takePicture ()
{
  std_msgs::Empty msg;
  ImageReceived = false;
  publish ("ImageReceived", ImageReceived);
  m_leftImageTriggerPublisher->publish (msg);
}

void OwInterface::digTrench (double x, double y, double z,
                             double depth, double length, double width,
                             double pitch, double yaw,
                             double dumpx, double dumpy, double dumpz)
{
  ROS_WARN("digTrench is unimplemented!");
}

double OwInterface::getTilt () const
{
  return CurrentTilt;
}

double OwInterface::getPanDegrees () const
{
  return CurrentPanDegrees;
}

double OwInterface::getPanVelocity () const
{
  return JointTelemetryMap[Joint::antenna_pan].velocity;
}

double OwInterface::getTiltVelocity () const
{
  return JointTelemetryMap[Joint::antenna_tilt].velocity;
}

bool OwInterface::imageReceived () const
{
  return ImageReceived;
}

bool OwInterface::serviceRunning (const string& name) const
{
  // Note: check in caller guarantees 'at' to return a valid value.
  return ServiceRunning.at(name)();
}

bool OwInterface::serviceFinished (const string& name) const
{
  return !serviceRunning(name);
}

bool OwInterface::running (const string& name) const
{
  if (is_service (name)) return serviceRunning(name);

  ROS_ERROR("OwInterface::running: unsupported operation: %s", name.c_str());
  return false;
}

bool OwInterface::finished (const string& name) const
{
  if (is_service (name)) return serviceFinished(name);

  ROS_ERROR("OwInterface::finished: unsupported operation: %s", name.c_str());
  return false;
}

bool OwInterface::hardTorqueLimitReached (const std::string& joint_name) const
{
  return (JointsAtHardTorqueLimit.find (joint_name) !=
          JointsAtHardTorqueLimit.end());
}

bool OwInterface::softTorqueLimitReached (const std::string& joint_name) const
{
  return (JointsAtSoftTorqueLimit.find (joint_name) !=
          JointsAtSoftTorqueLimit.end());
}
