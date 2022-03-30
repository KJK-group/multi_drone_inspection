#include <geometry_msgs/PointStamped.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/TwistStamped.h>
#include <mavros_msgs/CommandBool.h>
#include <mavros_msgs/SetMode.h>
#include <mavros_msgs/State.h>
#include <nav_msgs/Odometry.h>
#include <ros/ros.h>
#include <tf2/utils.h>
#include <tf2_eigen/tf2_eigen.h>
#include <tf2_ros/transform_listener.h>

#include <cmath>
#include <eigen3/Eigen/Dense>
#include <tuple>

#include "boost/format.hpp"

#define V_MAX 0.2
#define A 0.4
#define B 0.6
#define C -1.4
#define D -0.6
#define E 1.6
#define F -1.5

using namespace std;
using boost::format;
using boost::io::group;
using Eigen::Vector2f;
using Eigen::Vector3f;

// escape codes
auto magenta = "\u001b[35m";
auto green = "\u001b[32m";
auto reset = "\u001b[0m";
auto bold = "\u001b[1m";
auto italic = "\u001b[3m";
auto underline = "\u001b[4m";

// publishers
ros::Publisher pub_velocity;

// subscibers
ros::Subscriber sub_state;
ros::Subscriber sub_odom;

// services
ros::ServiceClient client_arm;
ros::ServiceClient client_mode;

// time
ros::Time start_time;

// transform utilities
tf2_ros::Buffer tf_buffer;

// sequence counters
auto seq_point_world = 0;
auto seq_point_body = 0;

// state variables
mavros_msgs::State state;

// targets
auto altitude_offset = 50.f;
auto subject_center = Vector3f(0.0f, 0.0f, move(altitude_offset));

// controller gains
auto k_alpha = 1.f;
auto k_rho = 1.f;

//--------------------------------------------------------------------------------------------------
// Polynomial Functions
//--------------------------------------------------------------------------------------------------

// 5th order trajectory function
auto trajectory(float x) -> float {
    return A * pow(x, 5) + B * pow(x, 4) + C * pow(x, 3) + D * pow(x, 2) + E * x + F;
}

// 5th order trajectory slope
auto trajectory_slope(float x) -> float {
    return 5 * A * pow(x, 4) + 4 * B * pow(x, 3) + 3 * pow(x, 2) + D * x + E;
}

//--------------------------------------------------------------------------------------------------
// Vector Functions
//--------------------------------------------------------------------------------------------------

auto scale = 1;

// circle vector function
auto circle_trajectory(float t) -> Vector2f {
    return Vector2f(scale * cos(V_MAX * t), scale * sin(V_MAX * t));
}

// 3D trajectory
auto circle_trajectory_3d(float t) -> Vector3f {
    return Vector3f(scale * cos(V_MAX * t), scale * sin(V_MAX * t), 0 * cos(t));
}

//--------------------------------------------------------------------------------------------------
// Callback Functions
//--------------------------------------------------------------------------------------------------

auto odom_cb(const nav_msgs::Odometry::ConstPtr& msg) -> void {
    // current position
    auto pos = msg->pose.pose.position;
    // current yaw
    auto yaw = tf2::getYaw(msg->pose.pose.orientation);
    // time diff
    auto delta_time = (ros::Time::now() - start_time).toNSec() / pow(10, 9);

    // heading error
    auto desired_heading = atan2(subject_center(1) - pos.y, subject_center(0) - pos.x);
    auto error_heading = desired_heading - yaw;
    // correct for magnitude larger than π
    if (error_heading > M_PI) {
        error_heading - 2 * M_PI;
    } else if (error_heading < -M_PI) {
        error_heading + 2 * M_PI;
    }

    // lookup transform
    auto frame_world = "PX4";
    auto frame_body = "PX4/odom_local_ned";
    geometry_msgs::TransformStamped transform;
    try {
        // transform from px4 drone odom to px4 world
        transform = tf_buffer.lookupTransform(frame_body, frame_world, ros::Time(0));
    } catch (tf2::TransformException& ex) {
        ROS_INFO("%s", ex.what());
        ros::Duration(1.0).sleep();
    }

    // get expected position
    auto expected_pos = circle_trajectory_3d(delta_time);
    expected_pos = Vector3f(1, 1, 1);
    // transform expected_pos to point_body_frame frame
    // point in world frame "PX4"
    auto point_world_frame = geometry_msgs::PointStamped();
    // fill header
    point_world_frame.header.seq = seq_point_world++;
    point_world_frame.header.stamp = ros::Time::now();
    point_world_frame.header.frame_id = frame_world;
    // fill point data
    point_world_frame.point.x = expected_pos(0);
    point_world_frame.point.y = expected_pos(1);
    point_world_frame.point.z = expected_pos(2);
    // point in point_body_frame frame "PX4/odom_local_ned"
    auto point_body_frame = geometry_msgs::PointStamped();
    // fill header
    point_body_frame.header.seq = seq_point_body++;
    point_body_frame.header.stamp = ros::Time::now();
    point_body_frame.header.frame_id = frame_body;
    // apply transform outputting result to point_body_frame
    tf2::doTransform(point_world_frame, point_body_frame, transform);
    auto expected_pos_body =
        Vector3f(point_world_frame.point.x, point_world_frame.point.y, point_world_frame.point.z);

    // position errors
    // auto error_x = expected_pos(0) - pos.x;
    // auto error_y = expected_pos(1) - pos.y;
    // auto error_z = expected_pos(2) + altitude_offset - pos.z;
    auto error_x = expected_pos_body(0);
    auto error_y = expected_pos_body(1);
    auto error_z = expected_pos_body(2) + altitude_offset - pos.z;

    // controller
    auto omega = k_alpha * error_heading;
    auto x_vel = k_rho * error_x;
    auto y_vel = k_rho * error_y;
    auto z_vel = k_rho * error_z;

    // control command
    geometry_msgs::TwistStamped command;
    // command.twist.angular.z = omega;
    // command.twist.linear.x = x_vel;
    // command.twist.linear.y = y_vel;
    // command.twist.linear.z = z_vel;
    pub_velocity.publish(command);

    // logging for debugging
    ROS_INFO_STREAM(magenta << "transform:\n" << transform << reset);
    ROS_INFO_STREAM(magenta << "from pose:\n" << point_world_frame << reset);
    ROS_INFO_STREAM(magenta << "to pose:\n" << point_body_frame << reset);
    // standard state logging
    ROS_INFO_STREAM(green << bold << italic << "errors:" << reset);
    ROS_INFO_STREAM("  heading: " << format("%1.5f") % group(setfill(' '), setw(8), error_heading));
    ROS_INFO_STREAM("  x:       " << format("%1.5f") % group(setfill(' '), setw(8), error_x));
    ROS_INFO_STREAM("  y:       " << format("%1.5f") % group(setfill(' '), setw(8), error_y));
    ROS_INFO_STREAM("  z:       " << format("%1.5f") % group(setfill(' '), setw(8), error_z));
    ROS_INFO_STREAM(green << bold << italic << "controller outputs:" << reset);
    ROS_INFO_STREAM("  omega: " << format("%1.5f") % group(setfill(' '), setw(8), omega));
    ROS_INFO_STREAM("  x_vel: " << format("%1.5f") % group(setfill(' '), setw(8), x_vel));
    ROS_INFO_STREAM("  y_vel: " << format("%1.5f") % group(setfill(' '), setw(8), y_vel));
    ROS_INFO_STREAM("  z_vel: " << format("%1.5f") % group(setfill(' '), setw(8), z_vel));
    ROS_INFO_STREAM(green << bold << italic << "time:" << reset);
    ROS_INFO_STREAM("  delta_time: " << format("%1.2f") % group(setfill(' '), setw(5), delta_time));
}


auto state_cb(const mavros_msgs::State::ConstPtr& msg) -> void { state = *msg; }

auto main(int argc, char** argv) -> int {
    // ROS initialisations
    ros::init(argc, argv, "mdi_test_controller");
    auto nh = ros::NodeHandle();
    ros::Rate rate(20.0);
    // save start_time
    start_time = ros::Time::now();

    // transform utilities
    tf2_ros::TransformListener tf_listener(tf_buffer);

    // state subsbricer
    sub_state = nh.subscribe<mavros_msgs::State>("/mavros/state", 10, state_cb);
    // odom subsbricer
    sub_odom = nh.subscribe<nav_msgs::Odometry>("/mavros/local_position/odom", 10, odom_cb);

    // velocity publisher
    pub_velocity =
        nh.advertise<geometry_msgs::TwistStamped>("/mavros/setpoint_velocity/cmd_vel", 10);

    // arm service client
    client_arm = nh.serviceClient<mavros_msgs::CommandBool>("/mavros/cmd/arming");
    // mode service client
    client_mode = nh.serviceClient<mavros_msgs::SetMode>("/mavros/set_mode");

    // wait for FCU connection
    while (ros::ok() && !state.connected) {
        ros::spinOnce();
        rate.sleep();
    }

    // arm the drone
    if (!state.armed) {
        mavros_msgs::CommandBool srv;
        srv.request.value = true;
        if (client_arm.call(srv)) {
            ROS_INFO("throttle armed: success");
        } else {
            ROS_INFO("throttle armed: fail");
        }
    }

    // set drone mode to OFFBOARD
    if (state.mode != "OFFBOARD") {
        mavros_msgs::SetMode mode_msg;
        mode_msg.request.custom_mode = "OFFBOARD";

        if (client_mode.call(mode_msg) && mode_msg.response.mode_sent) {
            ROS_INFO("mode set: OFFBOARD");
        } else {
            ROS_INFO("mode set: fail");
        }
    }

    // ROS spin
    while (ros::ok()) {
        ros::spinOnce();
        rate.sleep();
    }

    return 0;
}
