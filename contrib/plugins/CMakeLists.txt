add_library (qfsmonitorplugin SHARED qfs_monitor.cc)
set_target_properties (qfsmonitorplugin PROPERTIES OUTPUT_NAME "qfs_monitor")

add_library (jobmonitorplugin SHARED job_monitor.cc)
set_target_properties (jobmonitorplugin PROPERTIES OUTPUT_NAME "job_monitor")
