#include <ros/ros.h>
#include <moveit/move_group_interface/move_group.h>
#include <actionlib/server/simple_action_server.h>
#include <shape_msgs/SolidPrimitive.h>
#include <shape_tools/shape_extents.h>
#include <calvin_msgs/PickAndStoreAction.h>
#include <tf/tf.h>
#include <moveit_msgs/Grasp.h>
#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>

#include <cmath>

class PickServer {
  protected:
    ros::NodeHandle nh;
    actionlib::SimpleActionServer<calvin_msgs::PickAndStoreAction> *actionserver;
    moveit::planning_interface::MoveGroup *group;
    ros::Publisher grasps_marker;

    void publish_grasps_as_markerarray(std::vector<moveit_msgs::Grasp> grasps)
    {
      visualization_msgs::MarkerArray markers;
      int i = 0;

      for(std::vector<moveit_msgs::Grasp>::iterator it = grasps.begin(); it != grasps.end(); ++it) {
        visualization_msgs::Marker marker;
        marker.header.stamp = ros::Time::now();
        marker.header.frame_id = "base_footprint";
        marker.id = i;
        marker.type = 1;
        marker.ns = "graspmarker";
        marker.pose = it->grasp_pose.pose;
        marker.scale.x = 0.02;
        marker.scale.y = 0.02;
        marker.scale.z = 0.2;
        marker.color.b = 1.0;
        marker.color.a = 1.0;
        markers.markers.push_back(marker);
        i++;
      }

      grasps_marker.publish(markers);
    }

    moveit_msgs::Grasp build_grasp(tf::Transform t, const double close_width)
    {
      // distance between finger tip and joint
      static const double FINGER_LENGTH = 0.078;

      // assert (0 <= close_width && close_width < 2*FINGER_LENGTH );
      // angle to set for grasp to produce grasp width 'close_width'
      const double close_angle = asin( ( close_width/2 ) / FINGER_LENGTH ) - 0.44;

      static int i = 0;

      moveit_msgs::Grasp grasp;
      geometry_msgs::PoseStamped pose;

      tf::Vector3& origin = t.getOrigin();
      tf::Quaternion rotation = t.getRotation();

      tf::quaternionTFToMsg(rotation, pose.pose.orientation);

      pose.header.frame_id = "base_footprint";
      pose.header.stamp = ros::Time::now();
      pose.pose.position.x = origin.m_floats[0];
      pose.pose.position.y = origin.m_floats[1];
      pose.pose.position.z = origin.m_floats[2];
      grasp.grasp_pose = pose;

      grasp.id = boost::lexical_cast<std::string>(i);

      grasp.pre_grasp_approach.direction.vector.z = 1.0;
      grasp.pre_grasp_approach.direction.header.stamp = ros::Time::now();
      grasp.pre_grasp_approach.direction.header.frame_id = "katana_gripper_tool_frame";
      grasp.pre_grasp_approach.min_distance = 0.03;
      grasp.pre_grasp_approach.desired_distance = 0.07;

      grasp.post_grasp_retreat.direction.vector.z = 1.0;
      grasp.post_grasp_retreat.direction.header.stamp = ros::Time::now();
      grasp.post_grasp_retreat.direction.header.frame_id = "base_footprint";
      grasp.post_grasp_retreat.min_distance = 0.03;
      grasp.post_grasp_retreat.desired_distance = 0.07;

      grasp.pre_grasp_posture.joint_names.push_back("katana_l_finger_joint");
      grasp.pre_grasp_posture.joint_names.push_back("katana_r_finger_joint");
      grasp.pre_grasp_posture.points.resize(1);
      grasp.pre_grasp_posture.points[0].positions.push_back(0.3);
      grasp.pre_grasp_posture.points[0].positions.push_back(0.3);

      grasp.grasp_posture.joint_names.push_back("katana_l_finger_joint");
      grasp.grasp_posture.joint_names.push_back("katana_r_finger_joint");
      grasp.grasp_posture.points.resize(1);
      grasp.grasp_posture.points[0].positions.push_back(close_angle);
      grasp.grasp_posture.points[0].positions.push_back(close_angle);

      i++;
      return grasp;
    }

    /**
     * x, y, z: center of grasp point (the point that should be between the finger tips of the gripper)
     * close gripper to width
     */
    std::vector<moveit_msgs::Grasp> generate_grasps(double x, double y, double z, const double width)
    {
      static const double ANGLE_INC = M_PI / 16;
      static const double STRAIGHT_ANGLE_MIN = 0.0 + ANGLE_INC;  // + ANGLE_INC, because 0 is already covered by side grasps
      static const double ANGLE_MAX = M_PI / 2;

      // how far from the grasp center should the wrist be?
      static const double STANDOFF = -0.01;

      std::vector<moveit_msgs::Grasp> grasps;

      tf::Transform transform;

      tf::Transform standoff_trans;
      standoff_trans.setOrigin(tf::Vector3(STANDOFF, 0.0, 0.0));
      standoff_trans.setRotation(tf::Quaternion(0.0, sqrt(2)/2, 0.0, sqrt(2)/2));

      // ----- side grasps
      //
      //  1. side grasp (xy-planes of `katana_motor5_wrist_roll_link` and of `katana_base_link` are parallel):
      //     - standard: `rpy = (0, 0, *)` (orientation of `katana_motor5_wrist_roll_link` in `katana_base_link` frame)
      //     - overhead: `rpy = (pi, 0, *)`
      transform.setOrigin(tf::Vector3(x, y, z));

      for (double roll = 0.0; roll <= M_PI; roll += M_PI)
      {
        double pitch = 0.0;

        // add yaw = 0 first, then +ANGLE_INC, -ANGLE_INC, 2*ANGLE_INC, ...
        // reason: grasps with yaw near 0 mean that the approach is from the
        // direction of the arm; it is usually easier to place the object back like
        // this
        for (double yaw = ANGLE_INC; yaw <= ANGLE_MAX; yaw += ANGLE_INC)
        {
          // + atan2 to center the grasps around the vector from arm to object
          transform.setRotation(tf::createQuaternionFromRPY(roll, pitch, yaw + atan2(y, x)));
          grasps.push_back(build_grasp(transform * standoff_trans, width));

          if (yaw != 0.0)
          {
            transform.setRotation(tf::createQuaternionFromRPY(roll, pitch, -yaw + atan2(y, x)));
            grasps.push_back(build_grasp(transform * standoff_trans, width));
          }
        }
      }

      // ----- straight grasps
      //
      //  2. straight grasp (xz-plane of `katana_motor5_wrist_roll_link` contains z axis of `katana_base_link`)
      //     - standard: `rpy = (0, *, atan2(y_w, x_w))`   (x_w, y_w = position of `katana_motor5_wrist_roll_link` in `katana_base_link` frame)
      //     - overhead: `rpy = (pi, *, atan2(y_w, x_w))`
      for (double roll = 0.0; roll <= M_PI; roll += M_PI)
      {
        for (double pitch = STRAIGHT_ANGLE_MIN; pitch <= ANGLE_MAX; pitch += ANGLE_INC)
        {
          double yaw = atan2(y, x);
          transform.setOrigin(tf::Vector3(x, y, z));
          transform.setRotation(tf::createQuaternionFromRPY(roll, pitch, yaw));

          grasps.push_back(build_grasp(transform * standoff_trans, width));
        }
      }

      return grasps;
    }
  public:
    PickServer(std::string name) {
      group = new moveit::planning_interface::MoveGroup("arm");
      group->setPlanningTime(120.0);
      actionserver = new actionlib::SimpleActionServer<calvin_msgs::PickAndStoreAction>(nh, ros::names::resolve(name), boost::bind(&PickServer::pick, this, _1), false);
      grasps_marker = nh.advertise<visualization_msgs::MarkerArray>("grasps_marker", 10);
      actionserver->start();
    }
    ~PickServer() {
      delete actionserver;
      delete group;
    }
    void pick(const calvin_msgs::PickAndStoreGoalConstPtr &goal) {
      // maximum width of grasp (can't grasp anything bigger than this)
      static const double MAX_GRASP_WIDTH = 0.10;

      static const double SQUEEZE_FACTOR = 0.55;

      if(goal->co.primitive_poses.size() != 1 || goal->co.primitives.size() != 1){
        ROS_ERROR("PickAndStore requires a CollisionObject with exactly one SolidPrimitive. Aborting.");
        actionserver->setAborted();
        return;
      }

      double x = goal->co.primitive_poses[0].position.x;
      double y = goal->co.primitive_poses[0].position.y;
      double z = goal->co.primitive_poses[0].position.z;

      double width = 0.0;
      if( goal->close_gripper_partially ){
        // compute object 'width'
        // TODO: This ignores different object sizes from different approach directions and uses the front width for all of them
        double object_width, dx, dz;
        shape_tools::getShapeExtents(goal->co.primitives[0], dx, object_width, dz);

        width= SQUEEZE_FACTOR * object_width;
        if(0.0 > width || width > MAX_GRASP_WIDTH){
          width= std::min( MAX_GRASP_WIDTH, std::max(0.0, width) );
          ROS_WARN("Trying to pick an object which is too big to grasp (%fm), trying to pick with grasp width %fm", object_width, width);
        }
      }

      std::string id = goal->co.id;
      ROS_INFO("Trying to pick object %s at %f, %f, %f.", id.c_str(), x, y, z);
      bool result = group->pick(id, generate_grasps(x, y, z, width));
      if(result) {
          ROS_INFO("Pick Action succeeded. Trying to Place.");
          bool result = place(id);
          if(result) {
              ROS_INFO("Place Action succeeded.");
              actionserver->setSucceeded();
          }
          else {
              ROS_WARN("Place Action failed. Aborting.");
              actionserver->setAborted();
          }
      }
      else {
          ROS_WARN("Pick Action failed. Aborting.");
          actionserver->setAborted();
      }
    }
    bool place(std::string id) {
      static const double ANGLE_INC = M_PI / 16;

      std::vector<moveit_msgs::PlaceLocation> loc;

      for (double yaw = -M_PI; yaw < M_PI; yaw += ANGLE_INC)
      {
        geometry_msgs::PoseStamped p;
        p.header.frame_id = "base_footprint";
        p.pose.position.x = 0.21;
        p.pose.position.y = 0.115;
        p.pose.position.z = 0.78;
        tf::quaternionTFToMsg(tf::createQuaternionFromRPY(0.0, 0.0, yaw), p.pose.orientation);

        moveit_msgs::PlaceLocation g;
        g.place_pose = p;

        g.pre_place_approach.direction.vector.z = -1.0;
        g.pre_place_approach.direction.header.frame_id = "base_link";
        g.pre_place_approach.min_distance = 0.03;
        g.pre_place_approach.desired_distance = 0.07;
        g.post_place_retreat.direction.vector.z = 1.0;
        g.post_place_retreat.direction.header.frame_id = "base_link";
        g.post_place_retreat.min_distance = 0.03;
        g.post_place_retreat.desired_distance = 0.07;

        g.post_place_posture.joint_names.push_back("katana_l_finger_joint");
        g.post_place_posture.joint_names.push_back("katana_r_finger_joint");
        g.post_place_posture.points.resize(1);
        g.post_place_posture.points[0].positions.push_back(0.3);
        g.post_place_posture.points[0].positions.push_back(0.3);

        loc.push_back(g);
      }

      group->setSupportSurfaceName("cup");

      return group->place(id, loc);
    }
};

int main(int argc, char **argv) {
  ros::init (argc, argv, "calvin_pick_server");
  PickServer pickserver("calvin_pick_and_store");
  ros::spin();
  return 0;
}
