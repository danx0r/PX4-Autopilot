#!/usr/bin/env bash

source /opt/ros/humble/setup.bash
source ./source_me.sh

terminator --geometry 600x800+20+1600 -T "QGroundControl" -e './QGroundControl.AppImage' &

terminator --geometry 600x800+3200+1600 -T "gazebo server" -e 'gzserver --verbose /home/dbm/PX4-Autopilot/Tools/simulation/gazebo/sitl_gazebo/worlds/baylands.world' &

terminator --geometry 600x800+2600+1600 -T "gazebo client/PX4" -e 'bash run_gazebo.sh'
