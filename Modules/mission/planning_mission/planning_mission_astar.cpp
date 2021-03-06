//ros头文件
#include <ros/ros.h>
#include <Eigen/Eigen>
#include <iostream>
#include "mission_utils.h"
#include "message_utils.h"

//topic 头文件
#include <prometheus_msgs/ControlCommand.h>
#include <prometheus_msgs/DroneState.h>
#include <prometheus_msgs/PositionReference.h>
#include <geometry_msgs/PoseStamped.h>
#include <nav_msgs/Path.h>
#include <std_msgs/Int8.h>

using namespace std;

#define MIN_DIS 0.1
#define NODE_NAME "planning_mission_astar"
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>全 局 变 量<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
ros::Publisher command_pub;
ros::Publisher global_planner_switch_pub;
ros::Publisher message_pub;

prometheus_msgs::ControlCommand Command_Now;                               //发送给控制模块 [px4_pos_controller.cpp]的命令
prometheus_msgs::DroneState _DroneState;                                   //无人机状态量
geometry_msgs::PoseStamped goal;                              // goal                    
std_msgs::Int8 stop_cmd; 
std_msgs::Bool switch_on;
std_msgs::Bool switch_off;

struct global_planner
{
    // 规划路径
    nav_msgs::Path path_cmd;
    // 总路径点数
    int Num_total_wp;
    // 当前执行路径点
    int wp_id;  
    // 第一个执行的路径点
    int start_id;
}A_star;

double FLY_HEIGHT;
bool control_yaw_flag;
int flag_get_cmd = 0;
int flag_get_goal = 0;
float desired_yaw = 0;  //[rad]
float distance_to_goal = 0;
float wait_time = 0;
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>声 明 函 数<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
void A_star_planner();
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>回 调 函 数<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
void global_planner_cmd_cb(const nav_msgs::Path::ConstPtr& msg)
{
    flag_get_cmd = flag_get_cmd + 1;
    // 路径点
    A_star.path_cmd = *msg;
    // 路径点个数
    A_star.Num_total_wp = A_star.path_cmd.poses.size();

    // 选择与当前无人机所在位置最近的点,并从该点开始追踪
    A_star.start_id = 0;
    float distance_to_wp_min = abs(A_star.path_cmd.poses[0].pose.position.x - _DroneState.position[0])
                                + abs(A_star.path_cmd.poses[0].pose.position.y - _DroneState.position[1]);
    for (int j=1;j<A_star.Num_total_wp;j++)
    {
        float distance_to_wp = abs(A_star.path_cmd.poses[j].pose.position.x - _DroneState.position[0])
                                + abs(A_star.path_cmd.poses[j].pose.position.y - _DroneState.position[1]);
        if(distance_to_wp < distance_to_wp_min)
        {
            distance_to_wp_min = distance_to_wp;
            A_star.start_id = j;
        }
    }

    // 这里增大开始路径点是为了解决当得到新路径时,无人机会回头的问题
    A_star.wp_id = A_star.start_id + 1;
    // if(A_star.Num_total_wp - A_star.start_id > 8)
    // {
    //     A_star.wp_id = A_star.start_id + 7;
    // }
}
void stop_cmd_cb(const std_msgs::Int8::ConstPtr& msg)
{
    stop_cmd = *msg;
}

void drone_state_cb(const prometheus_msgs::DroneState::ConstPtr& msg)
{
    _DroneState = *msg;
    distance_to_goal = sqrt(  pow(_DroneState.position[0] - goal.pose.position.x, 2) 
                            + pow(_DroneState.position[1] - goal.pose.position.y, 2) );
}

void goal_cb(const geometry_msgs::PoseStamped::ConstPtr& msg)
{
    goal = *msg;
    flag_get_goal = 1;
    // 重新设定目标点后,认为当前无人机不危险
    stop_cmd.data == 0;
    cout << "Get a new goal!"<<endl;
}
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>主 函 数<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
int main(int argc, char **argv)
{
    ros::init(argc, argv, "planning_mission_astar");
    ros::NodeHandle nh("~");

    // 【参数】 执行路径时是否控制偏航角，建议设置为Fasle
    nh.param<bool>("planning_mission/control_yaw_flag", control_yaw_flag, false);
    // 【参数】 执行路径时的固定飞行高度，初次实验建议设置为0.5-1米
    nh.param<double>("planning_mission/FLY_HEIGHT", FLY_HEIGHT, 1.0);
    
    // 【订阅】 无人机当前状态
    ros::Subscriber drone_state_sub = nh.subscribe<prometheus_msgs::DroneState>("/prometheus/drone_state", 10, drone_state_cb);
    // 【订阅】 来自planning的指令：路径点
    ros::Subscriber global_planner_sub =    nh.subscribe<nav_msgs::Path>("/prometheus/global_planner/path_cmd", 50, global_planner_cmd_cb);
    // 【订阅】 来自planning的指令 ： 立刻停止指令
    ros::Subscriber stop_cmd_sub = nh.subscribe<std_msgs::Int8>("/prometheus/planning/stop_cmd", 10, stop_cmd_cb);  
    // 【订阅】 目标点
    ros::Subscriber goal_sub = nh.subscribe<geometry_msgs::PoseStamped>("/prometheus/planning/goal", 10,goal_cb);
    // 【发布】 发送给控制模块 [px4_pos_controller.cpp]的命令
    command_pub = nh.advertise<prometheus_msgs::ControlCommand>("/prometheus/control_command", 10);
    // 【发布】 用于地面站显示的提示消息
    message_pub = nh.advertise<prometheus_msgs::Message>("/prometheus/message/main", 10);
    // 【发布】 启动全局规划算法的指令
    global_planner_switch_pub = nh.advertise<std_msgs::Bool>("/prometheus/switch/global_planner", 10);
    stop_cmd.data = 0;
    switch_on.data = true;

    // 固定的浮点显示
    cout.setf(ios::fixed);
    // 设置显示小数精度为n位
    cout<<setprecision(3);
    // 左对齐
    cout.setf(ios::left);
    // 强制显示小数点
    cout.setf(ios::showpoint);
    // 强制显示符号
    cout.setf(ios::showpos);
    
    // Waiting for input
    int start_flag = 0;
    while(start_flag == 0)
    {
        cout << ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>Astar Planning Mission<<<<<<<<<<<<<<<<<<<<<<<<<<< "<< endl;
        cout << "Please input 1 for start:"<<endl;
        cin >> start_flag;

        if (start_flag == 1)
        {
            global_planner_switch_pub.publish(switch_on);
        }
    }

    // 起飞
    Command_Now.Command_ID = 1;
    Command_Now.source = NODE_NAME;

    Command_Now.header.stamp = ros::Time::now();
    Command_Now.Mode  = prometheus_msgs::ControlCommand::Idle;
    Command_Now.Command_ID = Command_Now.Command_ID + 1;
    Command_Now.source = NODE_NAME;
    Command_Now.Reference_State.yaw_ref = 999;
    command_pub.publish(Command_Now);   
    cout << "Switch to OFFBOARD and arm ..."<<endl;
    ros::Duration(3.0).sleep();
    
    Command_Now.header.stamp = ros::Time::now();
    Command_Now.Mode = prometheus_msgs::ControlCommand::Takeoff;
    Command_Now.Command_ID = Command_Now.Command_ID + 1;
    Command_Now.source = NODE_NAME;
    command_pub.publish(Command_Now);
    cout << "Takeoff ..."<<endl;
    ros::Duration(3.0).sleep();

    ros::spinOnce();
    

    while (ros::ok())
    {
        //回调
        ros::spinOnce();

        cout << ">>>>>>>>>>>>>>>>>>>>>>>>>>>> Astar Planning Mission <<<<<<<<<<<<<<<<<<<<<<<<<" <<endl;

        // 若goal为99，则降落并退出任务
        if(goal.pose.position.x == 99)
        {
            // 抵达目标附近，则停止速度控制，改为位置控制
            Command_Now.header.stamp = ros::Time::now();
            Command_Now.Mode                                = prometheus_msgs::ControlCommand::Land;
            Command_Now.Command_ID                          = Command_Now.Command_ID + 1;
            Command_Now.source = NODE_NAME;

            command_pub.publish(Command_Now);
            cout << "Quit... " << endl;

            return 0;
        }

        // 等待规划算法发布的指令
        if( flag_get_cmd == 0)
        {
            cout << "Waiting for trajectory" << endl;
        }
        // 抵达目标点附近后（MIN_DIS可在全局变量中修改），无人机将直接悬停于目标点
        else if (distance_to_goal < MIN_DIS)
        {
            // 抵达目标附近，则停止速度控制，改为位置控制
            Command_Now.header.stamp = ros::Time::now();
            Command_Now.Mode                                = prometheus_msgs::ControlCommand::Move;
            Command_Now.Command_ID                          = Command_Now.Command_ID + 1;
            Command_Now.source = NODE_NAME;
            Command_Now.Reference_State.Move_mode           = prometheus_msgs::PositionReference::XYZ_POS;
            Command_Now.Reference_State.Move_frame          = prometheus_msgs::PositionReference::ENU_FRAME;
            Command_Now.Reference_State.position_ref[0]     = goal.pose.position.x;
            Command_Now.Reference_State.position_ref[1]     = goal.pose.position.y;
            Command_Now.Reference_State.position_ref[2]     = FLY_HEIGHT;

            Command_Now.Reference_State.yaw_ref             = desired_yaw;
            command_pub.publish(Command_Now);
            cout << "Arrived the goal, waiting for a new goal... " << endl;
            cout << "drone_pos: " << _DroneState.position[0] << " [m] "<< _DroneState.position[1] << " [m] "<< _DroneState.position[2] << " [m] "<<endl;
            cout << "goal_pos: " << goal.pose.position.x << " [m] "<< goal.pose.position.y << " [m] "<< goal.pose.position.z << " [m] "<<endl;
            flag_get_goal = 0;
            // 等待新的目标点
            while (flag_get_goal == 0)
            {
                cout << "Waiting for trajectory" << endl;
                ros::spinOnce();
                ros::Duration(0.1).sleep();
            }
        }
        // 接收到紧急停止指令，无人机悬停于当前点
        else if(stop_cmd.data == 1)
        {
            Command_Now.header.stamp = ros::Time::now();
            Command_Now.Mode                                = prometheus_msgs::ControlCommand::Hold;
            Command_Now.Command_ID                          = Command_Now.Command_ID + 1;
            Command_Now.source = NODE_NAME;

            command_pub.publish(Command_Now);
            cout << "Dangerous! Hold there." << endl; 
            pub_message(message_pub, prometheus_msgs::Message::WARN, NODE_NAME, "Dangerous! Hold there.");

            ros::Duration(0.2).sleep();
        }else
        {
            // 运动阶段，执行规划指令
            A_star_planner();
            ros::Duration(0.05).sleep();
        }
    }

    return 0;

}

void A_star_planner()
{
    float current_cmd_id = flag_get_cmd;
    //执行给定路径点， 跳出循环的条件： 执行完所有当前路径点，或者收到了新的规划路线
    while( A_star.wp_id < A_star.Num_total_wp && flag_get_cmd == current_cmd_id)
    {
        // 若选择控制偏航角，则依据当前路径点及下一路径点的xy位置计算期望偏航角
        if (control_yaw_flag)
        {
            float next_desired_yaw;
            if(A_star.wp_id > 1)
            {            
                next_desired_yaw = atan2(A_star.path_cmd.poses[A_star.wp_id].pose.position.y - A_star.path_cmd.poses[A_star.wp_id -1].pose.position.y, 
                                        A_star.path_cmd.poses[A_star.wp_id].pose.position.x - A_star.path_cmd.poses[A_star.wp_id-1].pose.position.x);
            }else
            {
                next_desired_yaw = desired_yaw;
            }
            // 为防止偏航角变化过快，采用如下滤波方式平滑
            desired_yaw = (0.92*desired_yaw + 0.08*next_desired_yaw);
        }else
        {
            desired_yaw = 0.0;
        }

        Command_Now.header.stamp = ros::Time::now();
        Command_Now.Mode                                = prometheus_msgs::ControlCommand::Move;
        Command_Now.Command_ID                          = Command_Now.Command_ID + 1;
        Command_Now.source = NODE_NAME;
        Command_Now.Reference_State.Move_mode           = prometheus_msgs::PositionReference::XYZ_POS;
        Command_Now.Reference_State.Move_frame          = prometheus_msgs::PositionReference::ENU_FRAME;
        Command_Now.Reference_State.position_ref[0]     = A_star.path_cmd.poses[A_star.wp_id].pose.position.x;
        Command_Now.Reference_State.position_ref[1]     = A_star.path_cmd.poses[A_star.wp_id].pose.position.y;
        Command_Now.Reference_State.position_ref[2]     = A_star.path_cmd.poses[A_star.wp_id].pose.position.z;
        Command_Now.Reference_State.yaw_ref             = desired_yaw;
        
        command_pub.publish(Command_Now);
        cout << "A star planner:"<<endl;
        cout << "Iteration:  " << flag_get_cmd << endl;
        cout << "Moving to Waypoint: [ " << A_star.wp_id << " / "<< A_star.Num_total_wp<< " ] "<<endl;
        cout << "desired_point: "   << A_star.path_cmd.poses[A_star.wp_id].pose.position.x << " [m] "
                                    << A_star.path_cmd.poses[A_star.wp_id].pose.position.y << " [m] "
                                    << A_star.path_cmd.poses[A_star.wp_id].pose.position.z << " [m] "<<endl; 
        cout << "drone_pos: " << _DroneState.position[0] << " [m] "<< _DroneState.position[1] << " [m] "<< _DroneState.position[2] << " [m] "<<endl;
        cout << "goal_pos: " << goal.pose.position.x << " [m] "<< goal.pose.position.y << " [m] "<< goal.pose.position.z << " [m] "<<endl;
        
        
        ros::spinOnce();
        A_star.wp_id++;
        // 此处等待时间应当依据 无人机运动速度及路径间隔综合决定，方能保证无人机依次执行各个路径点
        wait_time = 0.6;
        ros::Duration(wait_time).sleep();
    }
}