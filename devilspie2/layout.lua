debug_print("Application: " .. get_application_name())
debug_print("Window: " .. get_window_name());

function sleep(n)
  os.execute("sleep " .. tonumber(n))
end

if (get_window_name() == "Mozilla Firefox") then
  set_window_geometry(700, 300, 1500, 1200);
  set_on_top();
end

if (get_application_name() == "rviz2") then
  sleep(1);
  unmaximize();
  sleep(1);
  set_on_top();
  sleep(1);
  set_window_geometry(1040, 25, 1600, 1000);
end

if (get_window_name() == "Gazebo") then
  sleep(1);
  unmaximize();
  sleep(1);
  set_on_top();
  sleep(1);
  set_window_geometry(2680, 25, 1200, 1000);
end

if (get_window_name() == "QGroundcontrol") then
  sleep(1);
  unmaximize();
  sleep(1);
  set_on_top();
  sleep(1);
  set_window_geometry(20, 25, 1000, 1000);
end
