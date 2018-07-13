#include <iostream>
#include <fstream>
#include <iomanip>
#include <dlfcn.h>
#include <sstream>

#include <pluginlib/class_list_macros.h>
#include "roscontrol-sot-controller.hh"


#if DEBUG
#define ODEBUG(x) std::cout << x << std::endl
#else
#define ODEBUG(x)
#endif
#define ODEBUG3(x) std::cout << x << std::endl

#define DBGFILE "/tmp/rcoh2sot.dat"

#define RESETDEBUG5() { std::ofstream DebugFile;	\
    DebugFile.open(DBGFILE,std::ofstream::out);		\
    DebugFile.close();}
#define ODEBUG5FULL(x) { std::ofstream DebugFile;	\
    DebugFile.open(DBGFILE,std::ofstream::app);		\
    DebugFile << __FILE__ << ":"			\
	      << __FUNCTION__ << "(#"			\
	      << __LINE__ << "):" << x << std::endl;	\
    DebugFile.close();}
#define ODEBUG5(x) { std::ofstream DebugFile;	\
    DebugFile.open(DBGFILE,std::ofstream::app); \
    DebugFile << x << std::endl;		\
    DebugFile.close();}

#define RESETDEBUG4()
#define ODEBUG4FULL(x)
#define ODEBUG4(x)

/// lhi: nickname for local_hardware_interface
/// Depends if we are on the real robot or not.

namespace lhi=hardware_interface;
using namespace lhi;

using namespace rc_sot_system;

namespace sot_controller  
{
  typedef std::map<std::string,std::string>::iterator it_map_rt_to_sot;

  EffortControlPDMotorControlData::EffortControlPDMotorControlData()
  {
    prev = 0.0; vel_prev = 0.0; des_pos=0.0;
    integ_err=0.0; 
  }
  
  void EffortControlPDMotorControlData::read_from_xmlrpc_value
  (const std::string &prefix)
  {
    pid_controller.initParam(prefix);
  }
   
  RCSotController::
  RCSotController():
    // Store 32 DoFs for 5 minutes (1 Khz: 5*60*1000)
    // -> 124 Mo of data.
    nonholonomic_base_controller_ (NULL),
    type_name_("RCSotController"),
    simulation_mode_(false),
    control_mode_(POSITION),
    accumulated_time_(0.0),
    jitter_(0.0)
  {
    RESETDEBUG4();
  }

  RCSotController::~RCSotController ()
  {
    if (nonholonomic_base_controller_) delete nonholonomic_base_controller_;
  }
  
  void RCSotController::
  displayClaimedResources(ClaimedResources & claimed_resources)
  {
#ifdef CONTROLLER_INTERFACE_KINETIC
    ClaimedResources::iterator it_claim;
    ROS_INFO_STREAM("Size of claimed resources: "<< claimed_resources.size());
    for (it_claim = claimed_resources.begin(); 
	 it_claim != claimed_resources.end(); 
	 ++it_claim)
      {
	hardware_interface::InterfaceResources & aclaim = *it_claim;
	ROS_INFO_STREAM("Claimed by RCSotController: " << aclaim.hardware_interface);
	
	for(std::set<std::string>::iterator
	      it_set_res=aclaim.resources.begin();
	    it_set_res!=aclaim.resources.end();
	    it_set_res++)
	  {
	    ROS_INFO_STREAM(" Resources belonging to the interface:" <<
			    *it_set_res);
	  }
	    
      }
#else
    std::set<std::string >::iterator it_claim;
    ROS_INFO_STREAM("Size of claimed resources: "<< claimed_resources.size());
    for (it_claim = claimed_resources.begin();
	 it_claim != claimed_resources.end();
	 ++it_claim)
      {
	std::string aclaim = *it_claim;
	ROS_INFO_STREAM("Claimed by RCSotController: " << aclaim);
      }
#endif
  }

  bool RCSotController::
  initRequest (lhi::RobotHW * robot_hw,
	       ros::NodeHandle &robot_nh,
	       ros::NodeHandle &controller_nh,
	       ClaimedResources & claimed_resources)
  {
    /// Read the parameter server
    if (!readParams(robot_nh))
      return false;

    /// Create ros control interfaces to hardware
    if (!initInterfaces(robot_hw,robot_nh,controller_nh,claimed_resources))
      return false;

    /// Create SoT
    SotLoaderBasic::Initialization();

    /// If we are in effort mode then the device should not do any integration.
    if (control_mode_==EFFORT)
      {
	sotController_->setNoIntegration();
	/// Fill desired position during the phase where the robot is waiting.
	for(unsigned int idJoint=0;idJoint<joints_.size();idJoint++)
	  {
	    std::string joint_name = joints_name_[idJoint];
	    std::map<std::string,EffortControlPDMotorControlData>::iterator
	      search_ecpd = effort_mode_pd_motors_.find(joint_name);
	    
	    if (search_ecpd!=effort_mode_pd_motors_.end())
	      {
		EffortControlPDMotorControlData & ecpdcdata =
		  search_ecpd->second;
		ecpdcdata.des_pos = joints_[idJoint].getPosition();
	      }
	  }
      }
    return true;
  }
  
  bool RCSotController::
  initInterfaces(lhi::RobotHW * robot_hw,
		 ros::NodeHandle & root_nh,
		 ros::NodeHandle & controller_nh,
		 ClaimedResources & claimed_resources)
  {
    std::string lns;
    lns="hardware_interface";

    // Check if construction finished cleanly
    if (state_!=CONSTRUCTED)
      {
	ROS_ERROR("Cannot initialize this controller because it failed to be constructed");
      }

    // Get a pointer to the joint position control interface
    pos_iface_ = robot_hw->get<PositionJointInterface>();
    if (! pos_iface_)
      {
	ROS_ERROR("This controller requires a hardware interface of type '%s'."
		  " Make sure this is registered in the %s::RobotHW class.",
		  getHardwareInterfaceType().c_str(), lns.c_str());
	return false ;
      }

    // Get a pointer to the joint velocity control interface
    vel_iface_ = robot_hw->get<VelocityJointInterface>();
    if (! vel_iface_)
      {
	ROS_ERROR("This controller requires a hardware interface of type '%s'."
		  " Make sure this is registered in the %s::RobotHW class.",
		  getHardwareInterfaceType().c_str(), lns.c_str());
	return false ;
      }

    // Get a pointer to the joint effort control interface
    effort_iface_ = robot_hw->get<EffortJointInterface>();
    if (! effort_iface_)
      {
	ROS_ERROR("This controller requires a hardware interface of type '%s'."
		  " Make sure this is registered in the %s::RobotHW class.",
		  getHardwareInterfaceType().c_str(),lns.c_str());
	    return false ;
      }

    // Get a pointer to the force-torque sensor interface
    ft_iface_ = robot_hw->get<ForceTorqueSensorInterface>();
    if (! ft_iface_ )
      {
	ROS_ERROR("This controller requires a hardware interface of type '%s '. " 
		  " Make sure this is registered inthe %s::RobotHW class.",
		  internal :: demangledTypeName<ForceTorqueSensorInterface>().c_str(),lns.c_str());
	return false ;
      }
    // Get a pointer to the IMU sensor interface
    imu_iface_ = robot_hw->get<ImuSensorInterface>();
    if (! imu_iface_)
      {
	ROS_ERROR("This controller requires a hardware interface of type '%s'."
		  " Make sure this is registered in the %s::RobotHW class.",
		  internal :: demangledTypeName<ImuSensorInterface>().c_str(),lns.c_str());
	return false ;
      }

    // Temperature sensor not available in simulation mode
    if (!simulation_mode_)
      {
	// Get a pointer to the actuator temperature sensor interface
	act_temp_iface_ = robot_hw->get<ActuatorTemperatureSensorInterface>();
	if (!act_temp_iface_)
	  {
	    ROS_ERROR("This controller requires a hardware interface of type '%s'."
		      " Make sure this is registered in the %s::RobotHW class.",
		      internal :: demangledTypeName<ActuatorTemperatureSensorInterface>().c_str(),lns.c_str());
	    return false ;	  
	  }
      }
	
      
    // Return which resources are claimed by this controller
    pos_iface_->clearClaims();
    vel_iface_->clearClaims();
    effort_iface_->clearClaims();
    
    if (!initNonHolonomicBaseController(robot_hw, root_nh, controller_nh, claimed_resources)) {
      return false;
    }
    if (! init ())
      {
	ROS_ERROR("Failed to initialize sot-controller" );
	std :: cerr << "FAILED LOADING SOT CONTROLLER" << std::endl;
	return false ;
      }
    ROS_INFO_STREAM("Initialization of interfaces for sot-controller Ok !");

#ifdef CONTROLLER_INTERFACE_KINETIC
    hardware_interface::InterfaceResources iface_res;
    iface_res.hardware_interface = hardware_interface::internal::demangledTypeName<PositionJointInterface>();
    iface_res.resources = pos_iface_->getClaims();
    claimed_resources.push_back(iface_res);
    
    displayClaimedResources(claimed_resources);
    pos_iface_->clearClaims();

    iface_res.hardware_interface = hardware_interface::internal::demangledTypeName<VelocityJointInterface>();
    iface_res.resources = vel_iface_->getClaims();
    claimed_resources.push_back(iface_res);
    
    displayClaimedResources(claimed_resources);
    vel_iface_->clearClaims();

    iface_res.hardware_interface = hardware_interface::internal::demangledTypeName<EffortJointInterface>();
    iface_res.resources = effort_iface_->getClaims();
    claimed_resources.push_back(iface_res);
    
    displayClaimedResources(claimed_resources);
    effort_iface_->clearClaims();
#else
    claimed_resources = pos_iface_->getClaims();
    displayClaimedResources(claimed_resources);
    pos_iface_->clearClaims();

    claimed_resources = vel_iface_->getClaims();
    displayClaimedResources(claimed_resources);
    vel_iface_->clearClaims();

    claimed_resources = effort_iface_->getClaims();
    displayClaimedResources(claimed_resources);
    effort_iface_->clearClaims();
#endif    

    ROS_INFO_STREAM("Initialization of sot-controller Ok !");
    // success
    state_ = INITIALIZED;

    return true;
  }
    
  bool RCSotController::
  init()
  {
    if (!initJoints()) 
      return false;
    if (!initIMU())
      return false;
    if (!initForceSensors())
      return false;
    if (!initTemperatureSensors())
      return false;

    // Initialize ros node.
    int argc=1;
    char *argv[1];
    argv[0] = new char[10];
    strcpy(argv[0],"libsot");
    SotLoaderBasic::initializeRosNode(argc,argv);
    
    return true;
  }

  bool RCSotController::
  readParamsSotLibName(ros::NodeHandle &robot_nh)
  {
    // Read param to find the library to load
    std::string dynamic_library_name;

   // Read libname
    if (!robot_nh.getParam("/sot_controller/libname",dynamic_library_name))
      {
	ROS_ERROR_STREAM("Could not read param /sot_controller/libname");
	if (robot_nh.hasParam("/sot_controller/libname")) 
	  {
	    ROS_ERROR_STREAM("Param /sot_controller/libname exists !");
	  }
	else
	  {
	    ROS_ERROR_STREAM("Param /sot_controller/libname does not exists !");
	    return false;
	  }
      }
    else
      {
	ROS_INFO_STREAM("Loading library name: " << dynamic_library_name);
      }
    /// SotLoaderBasic related method calls.
    // Initialize the dynamic_library_name for the sotLoader
    setDynamicLibraryName(dynamic_library_name);
    return true;
  }

  bool RCSotController::
  readParamsPositionControlData(ros::NodeHandle &robot_nh)
  {
    // Read libname
    if (robot_nh.hasParam("/sot_controller/position_control_init_pos"))
      {
       XmlRpc::XmlRpcValue xml_rpc_pci_pose;
       robot_nh.getParamCached("/sot_controller/position_control_init_pos",
                               xml_rpc_pci_pose);
       
       ROS_INFO("/sot_controller/position_control_init_pos: %d %d %d\n",
                xml_rpc_pci_pose.getType(),XmlRpc::XmlRpcValue::TypeArray,XmlRpc::XmlRpcValue::TypeStruct);
       desired_init_pose_.clear();
       
       for (size_t i=0;i<xml_rpc_pci_pose.size();i++)
         {
	   XmlRpc::XmlRpcValue xml_rpc_pci_el;

	   ROS_INFO("/sot_controller/position_control_init_pos/ %ld: %d %d %d\n",i,
		    xml_rpc_pci_pose[i].getType(),XmlRpc::XmlRpcValue::TypeArray,XmlRpc::XmlRpcValue::TypeStruct);
	   
           if (xml_rpc_pci_pose[i].hasMember("name"))
             {
	       std::string local_joint_name = std::string(xml_rpc_pci_pose[i]["name"]);
	       if (xml_rpc_pci_pose[i].hasMember("name"))
		 {
		   double local_des_pose = double(xml_rpc_pci_pose[i]["des_pos"]);
		   desired_init_pose_[local_joint_name] = local_des_pose;
		 }
	       else
		 {
		   ROS_INFO("parameter /sot_controller/position_control_init_pos/%ld  needs a desired position\n", i );
		   return false;
		 }
	     }
	   else
	     {
	       ROS_INFO("parameter /sot_controller/position_control_init_pos/%ld  needs a name\n",i  );
	       return false;
	     }
	 }
       return true;
      }
    ROS_ERROR("No parameter /sot_controller/position_control_init_pos");
    return false;

  }
  
  bool RCSotController::
  readParamsEffortControlPDMotorControlData(ros::NodeHandle &robot_nh)
  {
    // Read libname
    if (robot_nh.hasParam("/sot_controller/effort_control_pd_motor_init/gains"))
      {
       XmlRpc::XmlRpcValue xml_rpc_ecpd_init;
       robot_nh.getParamCached("/sot_controller/effort_control_pd_motor_init/gains",
                               xml_rpc_ecpd_init);
       
       ROS_INFO("/sot_controller/effort_control_pd_motor_init/gains: %d %d %d\n",
                xml_rpc_ecpd_init.getType(),XmlRpc::XmlRpcValue::TypeArray,XmlRpc::XmlRpcValue::TypeStruct);
       effort_mode_pd_motors_.clear();
       
       for (size_t i=0;i<joints_name_.size();i++)
         {
           if (xml_rpc_ecpd_init.hasMember(joints_name_[i]))
             {
               /*
               XmlRpc::XmlRpcValue &aDataSet= xml_rpc_ecpd_init[joints_name_[i]];
               ROS_INFO("/sot_controller/effort_control_pd_motor_init %s type: %d\n",joints_name_[i],aDataSet.getType());
               if (aDataSet.getType()!=XmlRpc::XmlRpcValue::TypeStruct)
                 {
                   ROS_ERROR("In /sot_controller/effort_control_pd_motor_init/gains/%s  not a struct"
                             ,joints_name_[i]);
                   throw XmlrpcHelperException("Pb in readParamsEffortControlPDMotorControlData");
                 }
               */
               std::string prefix= "/sot_controller/effort_control_pd_motor_init/gains/" + joints_name_[i];
               effort_mode_pd_motors_[joints_name_[i]].read_from_xmlrpc_value(prefix);
             }
           else
             ROS_INFO("joint %s not in /sot_controller/effort_control_pd_motor_init/gains\n",
                      joints_name_[i].c_str());
         }
       return true;
      }
    
    ROS_ERROR("No parameter /sot_controller/effort_controler_pd_motor_init");
    return false;
  }

  bool RCSotController::
  readParamsFromRCToSotDevice(ros::NodeHandle &robot_nh)
  {
    // Read libname
    if (robot_nh.hasParam("/sot_controller/map_rc_to_sot_device")) 
      {
	if (robot_nh.getParam("/sot_controller/map_rc_to_sot_device",
			      mapFromRCToSotDevice_))
	  {
	    /// TODO: Check if the mapping is complete wrt to the interface and the mapping.
	    ROS_INFO_STREAM("Loading map rc to sot device: ");
	    for (it_map_rt_to_sot it = mapFromRCToSotDevice_.begin(); 
		 it != mapFromRCToSotDevice_.end(); ++it) 
	      ROS_INFO_STREAM( it->first << ", " << it->second);
	  }
	else
	  {
	    ROS_ERROR_STREAM("Could not read param /sot_controller/map_rc_to_sot_device");
	    return false;
	  }
      }  
    else
      {
	ROS_ERROR_STREAM("Param /sot_controller/map_rc_to_sot_device does not exists !");
	return false;
      }
    return true;
  }

  bool RCSotController::
  readParamsJointNames(ros::NodeHandle &robot_nh)
  {
    /// Check if the /sot_controller/joint_names parameter exists.
    if (robot_nh.hasParam("/sot_controller/joint_names")) 
      {
	/// Read the joint_names list from this parameter
	robot_nh.getParam("/sot_controller/joint_names",
			  joints_name_);
	for(std::vector<std::string>::size_type i=0;i<joints_name_.size();i++)
	  {ROS_INFO_STREAM("joints_name_[" << i << "]=" << joints_name_[i]);}
	
      }
    else
      return false;

    /// Deduce from this the degree of freedom number.
    nbDofs_ = joints_name_.size();
    /// Initialize the size of the data to store. 
    DataOneIter_.init(nbDofs_,1);
    /// Initialize the data logger for 300s.
    RcSotLog.init(nbDofs_,300000);
	
    return true;
  }

  bool RCSotController::
  readParamsControlMode(ros::NodeHandle &robot_nh)
  {
    // Read param for the list of joint names.
    if (robot_nh.hasParam("/sot_controller/control_mode")) 
      {
	std::string scontrol_mode,seffort("EFFORT"),sposition("POSITION");

	/// Read the joint_names list
	robot_nh.getParam("/sot_controller/control_mode",scontrol_mode);
	ROS_INFO_STREAM("control mode read from param file:|" << scontrol_mode<<"|");
	
	if (scontrol_mode==seffort)
	  control_mode_ = EFFORT;
	else if (scontrol_mode==sposition)
	  control_mode_ = POSITION;
	else 
	  {
	    ROS_INFO_STREAM("Error in specifying control mode-> falls back to default position. Wrong control is:" << scontrol_mode);
	    std::string::size_type n;
	    n = scontrol_mode.find("EFFORT");
	    ROS_INFO_STREAM("n: " << n << " size: " << scontrol_mode.size() << " "<< sposition.size() << " " << seffort.size());
	    control_mode_ = POSITION;
	  }
      }
    else 
      ROS_INFO_STREAM("Default control mode : position");

    /// Always return true;
    return true;
  }

  bool RCSotController::
  readParamsdt(ros::NodeHandle &robot_nh)
  {
    /// Reading the jitter is optional but it is a very good idea.
    if (robot_nh.hasParam("/sot_controller/jitter"))
      {
	robot_nh.getParam("/sot_controller/jitter",jitter_);
	ROS_INFO_STREAM("jitter: " << jitter_);
      }

    /// Read /sot_controller/dt to know what is the control period
    if (robot_nh.hasParam("/sot_controller/dt"))
      {
	robot_nh.getParam("/sot_controller/dt",dt_);
	ROS_INFO_STREAM("dt: " << dt_);
	return true;
      }

    ROS_ERROR("You need to define a control period in param /sot_controller/dt");
    return false;
  }
  
  bool RCSotController::
  readParams(ros::NodeHandle &robot_nh)
  {

    /// Calls readParamsSotLibName
    // Reads the SoT dynamic library.
    if (!readParamsSotLibName(robot_nh))
      return false;

    /// Read /sot_controller/simulation_mode to know if we are in simulation mode
    // Defines if we are in simulation node.
    if (robot_nh.hasParam("/sot_controller/simulation_mode")) 
      simulation_mode_ = true;
    
    /// Calls readParamsJointNames
    // Reads the list of joints to be controlled.
    if (!readParamsJointNames(robot_nh))
      return false;

    /// Calls readParamsControlMode.
    // Defines if the control mode is position or effort
    readParamsControlMode(robot_nh);

    /// Calls readParamsFromRCToSotDevice
    // Mapping from ros-controll to sot device
    readParamsFromRCToSotDevice(robot_nh);

    /// Get control perioud
    if (!readParamsdt(robot_nh))
      return false;
    
    if (control_mode_==EFFORT)
      readParamsEffortControlPDMotorControlData(robot_nh);
    else if (control_mode_==POSITION)
      readParamsPositionControlData(robot_nh);
    return true;
  }

    
  bool RCSotController::
  initJoints()
  {
    // Init Joint Names.
    joints_.resize(joints_name_.size());
    
    // Joseph Mirabel: I do not remember why I
    // had to change nbDofs_ into joints_.size()
    // but I remember it caused some problems.
    for (unsigned int i=0;i<joints_.size();i++)
      {
	bool notok=true;
	SotControlMode lcontrol_mode = control_mode_;
	
	while (notok)
	  {
	    try 
	      {
		if (lcontrol_mode==POSITION)
		  {
		    joints_[i] = pos_iface_->getHandle(joints_name_[i]);
		    ROS_INFO_STREAM("Found joint " << joints_name_[i] << " in position.");
		  }
		else if (lcontrol_mode==EFFORT)
		  {
		    joints_[i] = effort_iface_->getHandle(joints_name_[i]);
		    ROS_INFO_STREAM("Found joint " << joints_name_[i] << " in effort.");
		  }

		// throws on failure
		notok=false;
	      }
	    catch (...)
	      {
		ROS_ERROR_STREAM("Could not find joint " 
				 << joints_name_[i]);
		if (lcontrol_mode==POSITION)
		  ROS_ERROR_STREAM(" in POSITION");
		else
		  ROS_ERROR_STREAM(" in EFFORT");
		  
		if (lcontrol_mode==POSITION)
		  return false ;	
		else if (lcontrol_mode==EFFORT)
		  lcontrol_mode = POSITION;
	      }
	  }
      }
        
    return true ;
    
  }
  
  bool RCSotController::
  initIMU()
  {
    // get all imu sensor names
    const std :: vector<std :: string >& imu_iface_names = imu_iface_->getNames();
    for (unsigned i=0; i <imu_iface_names.size(); i++)
      ROS_INFO("Got sensor %s", imu_iface_names[i].c_str());
    for (unsigned i=0; i <imu_iface_names.size(); i++){
      // sensor handle on imu
      imu_sensor_.push_back(imu_iface_->getHandle(imu_iface_names[i]));
    }
 
    return true ;
  }
  
  bool RCSotController::
  initForceSensors()
  {
    // get force torque sensors names package.
    const std::vector<std::string>& ft_iface_names = ft_iface_->getNames();
    for (unsigned i=0; i <ft_iface_names.size(); i++)
      ROS_INFO("Got sensor %s", ft_iface_names[i].c_str());
    for (unsigned i=0; i <ft_iface_names.size(); i++){
      // sensor handle on torque forces
      ft_sensors_.push_back(ft_iface_->getHandle(ft_iface_names[i]));
    }
    return true;
  }

  bool RCSotController::
  initTemperatureSensors()
  {
    if (!simulation_mode_)
      {
	// get temperature sensors names
	const std::vector<std::string>& act_temp_iface_names = act_temp_iface_->getNames();
	ROS_INFO("Actuator temperature sensors: %ld",act_temp_iface_names.size() ); 
	
	for (unsigned i=0; i <act_temp_iface_names.size(); i++)
	  ROS_INFO("Got sensor %s", act_temp_iface_names[i].c_str());
	for (unsigned i=0; i <act_temp_iface_names.size(); i++){
	  // sensor handle on actuator temperature
	  act_temp_sensors_.push_back(act_temp_iface_->getHandle(act_temp_iface_names[i]));
	}
      }

    return true;
  }
  
  bool RCSotController::
  initNonHolonomicBaseController(lhi::RobotHW * robot_hw,
                                 ros::NodeHandle& root_nh,
                                 ros::NodeHandle &,
                                 ClaimedResources & claimed_resources)
  {
    /// Check if the base controller should be initialized.
    if (root_nh.hasParam("/sot_controller/mobile_base_controller"))
      {
        std::string param;
        if (root_nh.getParam("/sot_controller/mobile_base_controller", param))
          {
            nonholonomic_base_controller_ = new diff_drive_controller::DiffDriveController ();
            ros::NodeHandle controller_nh (param);
            nonholonomic_base_controller_->init (vel_iface_, root_nh, controller_nh);
            ((controller_interface::ControllerBase*)nonholonomic_base_controller_)->initRequest (robot_hw, root_nh, controller_nh, claimed_resources);
          }
        else
          {
	    ROS_ERROR_STREAM("Could not read param /sot_controller/mobile_base_controller");
            return false;
          }
      }
    else
      nonholonomic_base_controller_ = NULL;
    return true;
  }

  void RCSotController::
  fillSensorsIn(std::string &title, std::vector<double> & data)
  {
    /// Tries to find the mapping from the local validation
    /// to the SoT device.
    it_map_rt_to_sot it_mapRC2Sot= mapFromRCToSotDevice_.find(title);
    /// If the mapping is found
    if (it_mapRC2Sot!=mapFromRCToSotDevice_.end())
      {
	/// Expose the data to the SoT device.
	std::string lmapRC2Sot = it_mapRC2Sot->second;
	sensorsIn_[lmapRC2Sot].setName(lmapRC2Sot);
	sensorsIn_[lmapRC2Sot].setValues(data);
      }
  }

  void RCSotController::
  fillJoints()
  {
    /// Fill positions, velocities and torques.
    for(unsigned int idJoint=0;idJoint<joints_.size();idJoint++)
      {
	DataOneIter_.motor_angle[idJoint] = joints_[idJoint].getPosition();
	//if (!simulation_mode_)
	  DataOneIter_.joint_angle[idJoint] = joints_[idJoint].getAbsolutePosition();

	DataOneIter_.velocities[idJoint] = joints_[idJoint].getVelocity();
	//if (!simulation_mode_)
	  DataOneIter_.torques[idJoint] = joints_[idJoint].getTorqueSensor();
	DataOneIter_.motor_currents[idJoint] = joints_[idJoint].getEffort();
      }
    
    /// Update SoT internal values
    std::string ltitle("motor-angles"); 
    fillSensorsIn(ltitle,DataOneIter_.motor_angle);
    ltitle = "joint-angles";
    fillSensorsIn(ltitle,DataOneIter_.joint_angle);
    ltitle = "velocities";
    fillSensorsIn(ltitle,DataOneIter_.velocities);
    ltitle = "torques";
    fillSensorsIn(ltitle,DataOneIter_.torques);
    ltitle = "currents";
    fillSensorsIn(ltitle,DataOneIter_.motor_currents);
    
  }

  void RCSotController::
  fillNonHolonomicBase(const ros::Time& time, const ros::Duration& period)
  {
    if (nonholonomic_base_controller_) {
      nonholonomic_base_controller_->computeOdom(time, period);
      const diff_drive_controller::Odometry& odom = nonholonomic_base_controller_->odometry();
      DataOneIter_.odometry[0] = odom.getX();
      DataOneIter_.odometry[1] = odom.getY();
      DataOneIter_.odometry[2] = odom.getHeading();
      // Set base position
      std::string title ("odometry");
      fillSensorsIn (title, DataOneIter_.odometry);
    }
  }

  void RCSotController::setSensorsImu(std::string &name,
					   int IMUnb,
					   std::vector<double> & data)
  {
    std::ostringstream labelOss;
    labelOss << name << IMUnb;
    std::string label_s = labelOss.str();
    fillSensorsIn(label_s,data);
  }

  void RCSotController::
  fillImu()
  {
    for(unsigned int idIMU=0;idIMU<imu_sensor_.size();idIMU++)
      {
	/// Fill orientations, gyrometer and acceleration from IMU.
	if (imu_sensor_[idIMU].getOrientation())
	  {
	    for(unsigned int idquat = 0;idquat<4;idquat++)
	      {
		DataOneIter_.orientation[idquat] = imu_sensor_[idIMU].getOrientation ()[idquat];
	      }
	  }
	if (imu_sensor_[idIMU].getAngularVelocity())
	  {
	    for(unsigned int idgyrometer = 0;idgyrometer<3;
		idgyrometer++)
	      {
		DataOneIter_.gyrometer[idgyrometer] = 
		  imu_sensor_[idIMU].getAngularVelocity()[idgyrometer];
	      }
	  }
	if (imu_sensor_[idIMU].getLinearAcceleration())
	  {
	    for(unsigned int idlinacc = 0;idlinacc<3;
		idlinacc++)
	      {
		DataOneIter_.accelerometer[idlinacc] = 
		  imu_sensor_[idIMU].getLinearAcceleration()[idlinacc];
	      }
	  }
	
	std::string orientation_s("orientation_");
	setSensorsImu(orientation_s, idIMU, DataOneIter_.orientation);

	std::string gyrometer_s("gyrometer_");
	setSensorsImu(gyrometer_s, idIMU, DataOneIter_.gyrometer);

	std::string accelerometer_s("accelerometer_");
	setSensorsImu(accelerometer_s, idIMU, DataOneIter_.accelerometer);
      }
  }
  
  void RCSotController::
  fillForceSensors()
  {
    for(unsigned int idFS=0;idFS<ft_sensors_.size();
	idFS++)
      {
	for(unsigned int idForce=0;idForce<3;idForce++)
	  DataOneIter_.force_sensors[idFS*6+idForce]=
	    ft_sensors_[idFS].getForce()[idForce];
	for(unsigned int idTorque=0;idTorque<3;idTorque++)
	  DataOneIter_.force_sensors[idFS*6+3+idTorque]=
	    ft_sensors_[idFS].getTorque()[idTorque];
      }

    
    std::string alabel("forces");
    fillSensorsIn(alabel,DataOneIter_.force_sensors);
  }

  void RCSotController::
  fillTempSensors()
  {
    if (!simulation_mode_)
      {
	for(unsigned int idFS=0;idFS<act_temp_sensors_.size();idFS++)
	  {
	    DataOneIter_.temperatures[idFS]=  act_temp_sensors_[idFS].getValue();
	  }
      }
    else
      {
	for(unsigned int idFS=0;idFS<nbDofs_;idFS++)
	  DataOneIter_.temperatures[idFS]=  0.0;
      }

    std::string alabel("act-temp");
    fillSensorsIn(alabel,DataOneIter_.temperatures);
  }

  void RCSotController::
  fillSensors(const ros::Time& time, const ros::Duration& period)
  {
    fillJoints();
    fillNonHolonomicBase(time, period);
    fillImu();
    fillForceSensors();
    fillTempSensors();
  }
  
  void RCSotController::
  readControl(const ros::Time& time, const ros::Duration& period,
      std::map<std::string,dgs::ControlValues> &controlValues)
  {
    ODEBUG4("joints_.size() = " << joints_.size());
	    
    std::string cmdTitle;
    if (control_mode_==POSITION)
      cmdTitle="cmd-joints";
    else if (control_mode_==EFFORT)
      cmdTitle="cmd-effort";

    if (nonholonomic_base_controller_)
      {
        it_map_rt_to_sot it_mapRC2Sot= mapFromRCToSotDevice_.find("cmd-base-vel");
        if (it_mapRC2Sot!=mapFromRCToSotDevice_.end())
          {
            std::string lmapRC2Sot = it_mapRC2Sot->second;
            const std::vector<double>& command = controlValues[lmapRC2Sot].getValues();
            assert (command.size() == 2);
            base_command_.linear .x = command[0];
            base_command_.angular.z = command[1];
            nonholonomic_base_controller_->cmdVelCallback(base_command_);
            nonholonomic_base_controller_->computeCommand (time, period);
          }
      }

    it_map_rt_to_sot it_mapRC2Sot= mapFromRCToSotDevice_.find(cmdTitle);
    if (it_mapRC2Sot!=mapFromRCToSotDevice_.end())
      {
	std::string lmapRC2Sot = it_mapRC2Sot->second;
	command_ = controlValues[lmapRC2Sot].getValues();
	ODEBUG4("angleControl_.size() = " << command_.size());
  // Joseph Mirabel: Same as above. I don't remember
  // why it's important.
	for(unsigned int i=0;
	    // i<command_.size();++i)
	    i<joints_.size();++i)
	  {
	    joints_[i].setCommand(command_[i]);
	  }
      }
  }

  void RCSotController::one_iteration(const ros::Time& time, const ros::Duration& period)
  {
    // Chrono start
    RcSotLog.start_it();
    
    /// Update the sensors.
    fillSensors(time, period);

    /// Generate a control law.
    try
      {
	sotController_->nominalSetSensors(sensorsIn_);
	sotController_->getControl(controlValues_);
      }
    catch(std::exception &e) { throw e;}

    /// Read the control values
    readControl(time, period, controlValues_);
    
    // Chrono stop.
    RcSotLog.stop_it();
    
    /// Store everything in Log.
    RcSotLog.record(DataOneIter_);
  }

  void RCSotController::
  localStandbyEffortControlMode(const ros::Duration& period)
  {
    // ROS_INFO("Compute command for effort mode: %d %d",joints_.size(),effort_mode_pd_motors_.size());
    for(unsigned int idJoint=0;idJoint<joints_.size();idJoint++)
      {
	std::string joint_name = joints_name_[idJoint];
	std::map<std::string,EffortControlPDMotorControlData>::iterator
	  search_ecpd = effort_mode_pd_motors_.find(joint_name);
             
	if (search_ecpd!=effort_mode_pd_motors_.end())
	  {
	    EffortControlPDMotorControlData & ecpdcdata =
	      search_ecpd->second;
	    double vel_err = 0 - joints_[idJoint].getVelocity();
	    double err = ecpdcdata.des_pos - joints_[idJoint].getPosition();
            
	    ecpdcdata.integ_err +=err;
	    
	    double local_command = ecpdcdata.pid_controller.computeCommand(err,vel_err,period);
	    // Apply command
	    control_toolbox::Pid::Gains gains = ecpdcdata.pid_controller.getGains();
	    //ROS_INFO("command: %d %s %f %f (%f %f %f)",idJoint, joints_name_[idJoint].c_str(),
	    //local_command, DataOneIter_.motor_angle[idJoint],
	    //gains.p_gain_,gains.d_gain_, gains.i_gain_);
	    joints_[idJoint].setCommand(local_command);
            
	    // Update previous value.
	    ecpdcdata.prev = DataOneIter_.motor_angle[idJoint];
	  }
      }
  }

  void RCSotController::
  localStandbyPositionControlMode()
  {
    /// Iterate over all the joints
    for(unsigned int idJoint=0;idJoint<joints_.size();idJoint++)
      {
	/// Find the joint
	std::string joint_name = joints_name_[idJoint];
	/// Find the related data
	std::map<std::string,double>::iterator
	  search_ecpd = desired_init_pose_.find(joint_name);
	
	if (search_ecpd!=desired_init_pose_.end())
	  {
	    double ecpdcdata = search_ecpd->second;
	    /// Control the joint accordingly
	    joints_[idJoint].setCommand(ecpdcdata);
	  }
	else
	  ROS_ERROR("Unable to find init pose for joint %s", joint_name.c_str());
      }
  }
  
  void RCSotController::
  update(const ros::Time& time, const ros::Duration& period)
   {
    // Do not send any control if the dynamic graph is not started
     if (!isDynamicGraphStopped())
      {
       try
         {
	   double periodInSec = period.toSec();
	   if (periodInSec+accumulated_time_>dt_-jitter_)
	     {
	       one_iteration(time, period);
	       accumulated_time_ = 0.0;
	     }
	   else
	     accumulated_time_ += periodInSec;
         }
       catch (std::exception const &exc)
         {
           std::cerr << "Failure happened during one_iteration evaluation: std_exception" << std::endl;
           std::cerr << "Use gdb on this line together with gdb to investiguate the problem: " <<std::endl;
           std::cerr << __FILE__ << " " << __LINE__  << std::endl;
           throw exc;
         }
       catch (...)
         {
           std::cerr << "Failure happened during one_iteration evaluation: unknown exception" << std::endl;
           std::cerr << "Use gdb on this line together with gdb to investiguate the problem: " <<std::endl;
           std::cerr << __FILE__ << " " << __LINE__  << std::endl;
         }
      }
    else
      // But in effort mode it means that we are sending 0
      // Therefore implements a default PD controller on the system.
      if (control_mode_==EFFORT)
	localStandbyEffortControlMode(period);
      else if (control_mode_==POSITION)
	localStandbyPositionControlMode();
   }
  
  void RCSotController::
  starting(const ros::Time& time)
  {
    ros::Duration unused;
    fillSensors(time, unused);
    if (nonholonomic_base_controller_) nonholonomic_base_controller_->startRequest (time);
  }
    
  void RCSotController::
  stopping(const ros::Time & time)
  {
    if (nonholonomic_base_controller_) nonholonomic_base_controller_->stopRequest (time);
    std::string afilename("/tmp/sot.log");
    RcSotLog.save(afilename);
  }
  
  std::string RCSotController::
  getHardwareInterfaceType() const
  {
    //return type_name_;
    if (control_mode_==POSITION)
      return lhi::internal::
	demangledTypeName<lhi::PositionJointInterface>();
    else if (control_mode_==EFFORT)
      return lhi::internal::
	demangledTypeName<lhi::EffortJointInterface>();
    std::string voidstring("");
    return voidstring;
  }
  

  PLUGINLIB_EXPORT_CLASS(sot_controller::RCSotController, 
			 lci::ControllerBase);
}
