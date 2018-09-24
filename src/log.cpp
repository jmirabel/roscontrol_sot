/* 
   Olivier Stasse CNRS,
   31/07/2012
   Object to log the low-level informations of a robot.
*/
#include "log.hh"
#include <sys/time.h>
#include <sstream>
#include <fstream>
#include <iostream>
#include <iomanip>

using namespace std;
using namespace rc_sot_system;

DataToLog::DataToLog()
{
}

void DataToLog::init(unsigned int nbDofs,long int length)
{
  motor_angle.resize(nbDofs*length);
  joint_angle.resize(nbDofs*length);
  velocities.resize(nbDofs*length);
  torques.resize(nbDofs*length);
  motor_currents.resize(nbDofs*length);
  orientation.resize(4*length);
  accelerometer.resize(3*length);
  gyrometer.resize(3*length);
  force_sensors.resize(4*6*length);
  temperatures.resize(nbDofs*length);
  timestamp.resize(length);
  duration.resize(length);

  for(unsigned int i=0;i<nbDofs*length;i++)
    { motor_angle[i] = joint_angle[i] = 
	velocities[i] = 0.0;  }

}


Log::Log():  
  lref_(0),
  lrefts_(0)
{
}

void Log::init(unsigned int nbDofs, unsigned int length)
{
  lref_ =0;
  lrefts_=0;
  nbDofs_=nbDofs;
  length_=length;
  StoredData_.init(nbDofs,length);
  struct timeval current;
  gettimeofday(&current,0);

  timeorigin_ = (double)current.tv_sec + 0.000001 * ((double)current.tv_usec);

}

void Log::record(DataToLog &aDataToLog)
{
  if ( (aDataToLog.motor_angle.size()!=nbDofs_) ||
       (aDataToLog.velocities.size()!=nbDofs_))
    return;

  for(unsigned int JointID=0;JointID<nbDofs_;JointID++)
    {
      if (aDataToLog.motor_angle.size()>JointID)
	StoredData_.motor_angle[JointID+lref_]= aDataToLog.motor_angle[JointID];
      if (aDataToLog.joint_angle.size()>JointID)
	StoredData_.joint_angle[JointID+lref_]= aDataToLog.joint_angle[JointID];
      if (aDataToLog.velocities.size()>JointID)
	StoredData_.velocities[JointID+lref_]= aDataToLog.velocities[JointID];
      if (aDataToLog.torques.size()>JointID)
	StoredData_.torques[JointID+lref_]= aDataToLog.torques[JointID];
      if (aDataToLog.motor_currents.size()>JointID)
	StoredData_.motor_currents[JointID+lref_]= aDataToLog.motor_currents[JointID];
      if (aDataToLog.temperatures.size()>JointID)
	StoredData_.temperatures[JointID+lref_]= aDataToLog.temperatures[JointID];
    }
  for(unsigned int axis=0;axis<3;axis++)
    {
      StoredData_.accelerometer[lrefts_*3+axis] = aDataToLog.accelerometer[axis];
      StoredData_.gyrometer[lrefts_*3+axis] = aDataToLog.gyrometer[axis];
    }
  for(unsigned int fsID=0;fsID<4;fsID++)
    {
      for(unsigned int axis=0;axis<6;axis++)
	{
	  StoredData_.force_sensors[lrefts_*24+fsID*6+axis] = 
	    aDataToLog.force_sensors[fsID*6+axis];
	}
    }
  struct timeval current;
  gettimeofday(&current,0);

  StoredData_.timestamp[lrefts_] = 
    ((double)current.tv_sec + 0.000001 * (double)current.tv_usec) - timeorigin_;

  StoredData_.duration[lrefts_] = time_stop_it_ - time_start_it_;
    
  lref_ += nbDofs_;
  lrefts_ ++;
  if (lref_>=nbDofs_*length_)
    {
      lref_=0;
      lrefts_=0;
    }
}

void Log::start_it()
{
  struct timeval current;
  gettimeofday(&current,0);

  time_start_it_ = 
    ((double)current.tv_sec + 0.000001 * (double)current.tv_usec) - timeorigin_;
  
}

void Log::stop_it()
{
  struct timeval current;
  gettimeofday(&current,0);

  time_stop_it_ = 
    ((double)current.tv_sec + 0.000001 * (double)current.tv_usec) - timeorigin_;
}

void Log::save(std::string &fileName)
{
  std::string suffix("-mastate.log");
  saveVector(fileName,suffix,StoredData_.motor_angle, nbDofs_);
  suffix="-jastate.log";
  saveVector(fileName,suffix,StoredData_.joint_angle, nbDofs_);
  suffix = "-vstate.log";
  saveVector(fileName,suffix,StoredData_.velocities, nbDofs_);
  suffix = "-torques.log";
  saveVector(fileName,suffix,StoredData_.torques, nbDofs_);
  suffix = "-motor-currents.log";
  saveVector(fileName,suffix,StoredData_.motor_currents, nbDofs_);
  suffix = "-accelero.log";
  saveVector(fileName,suffix,StoredData_.accelerometer, 3);
  suffix = "-gyro.log";
  saveVector(fileName,suffix,StoredData_.gyrometer, 3);

  ostringstream oss;
  oss << "-forceSensors.log";
  suffix = oss.str();
  saveVector(fileName,suffix,StoredData_.force_sensors, 6);

  suffix = "-temperatures.log";
  saveVector(fileName,suffix,StoredData_.temperatures, nbDofs_);

  suffix = "-duration.log";
  saveVector(fileName,suffix,StoredData_.duration, 1);

}

void writeToBuffer (ofstream& of, char* buffer,
    const unsigned int& bufferSize, unsigned int& cursor,
    const double& d)
{
  if (bufferSize < cursor + 512) {
    of.write (buffer, cursor);
    cursor = 0;
  }
  const char dblFmt[] = "%14.12e ";
  cursor += sprintf (&buffer[cursor], dblFmt, d);
}

void Log::saveVector(std::string &fileName,std::string &suffix,
		     const std::vector<double> &avector,
		     unsigned int size)
{
  ostringstream oss;
  oss << fileName;
  oss << suffix.c_str();
  std::string actualFileName= oss.str();

  ofstream aof(actualFileName.c_str());

  // Increase speed to write to hard disk.
  // See https://stackoverflow.com/q/12997131
  const unsigned int length = 1 << 19; // 512 kB
  char buffer[length];

  unsigned int cursor=0;
  if (aof.is_open())
    {
      for(unsigned long int i=0;i<length_;i++)
	{
	  // Save timestamp
          writeToBuffer (aof, buffer, length, cursor, StoredData_.timestamp[i]);

	  // Compute and save dt
	  if (i==0)
            writeToBuffer (aof, buffer, length, cursor, StoredData_.timestamp[i] - StoredData_.timestamp[length_-1]);
	  else
            writeToBuffer (aof, buffer, length, cursor, StoredData_.timestamp[i] - StoredData_.timestamp[i-1]);

	  // Save all data
	  for(unsigned long int datumID=0;datumID<size;datumID++)
            writeToBuffer (aof, buffer, length, cursor, avector[i*size+datumID]);

          buffer[cursor] = '\n';
          ++cursor;
	}
      aof.close();
    }
}
