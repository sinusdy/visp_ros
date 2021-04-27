/*
 * \example tutorial-franka-coppeliasim-pbvs-apriltag.cpp
 */

#include <iostream>
#include <mutex>

#include <visp3/core/vpCameraParameters.h>
#include <visp3/core/vpImagePoint.h>
#include <visp3/gui/vpDisplayOpenCV.h>
#include <visp3/io/vpImageIo.h>
#include <visp3/detection/vpDetectorAprilTag.h>
#include <visp3/visual_features/vpFeatureThetaU.h>
#include <visp3/visual_features/vpFeatureTranslation.h>
#include <visp3/vs/vpServo.h>
#include <visp3/vs/vpServoDisplay.h>
#include <visp3/gui/vpPlot.h>

#include <ros/ros.h>
#include <geometry_msgs/TwistStamped.h>
#include <geometry_msgs/WrenchStamped.h>
#include <geometry_msgs/PoseStamped.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
#include <std_msgs/Bool.h>
#include <std_msgs/Float32.h>

#include <visp_ros/vpROSGrabber.h>
#include <visp_ros/vpROSRobotFrankaSim.h>

static bool s_simStepDone = true;
static std::mutex s_mutex_ros;
static float s_simTime = 0;

void display_point_trajectory(const vpImage<unsigned char> &I, const std::vector<vpImagePoint> &vip,
                              std::vector<vpImagePoint> *traj_vip)
{
  for (size_t i = 0; i < vip.size(); i++) {
    if (traj_vip[i].size()) {
      // Add the point only if distance with the previous > 1 pixel
      if (vpImagePoint::distance(vip[i], traj_vip[i].back()) > 1.) {
        traj_vip[i].push_back(vip[i]);
      }
    }
    else {
      traj_vip[i].push_back(vip[i]);
    }
  }
  for (size_t i = 0; i < vip.size(); i++) {
    for (size_t j = 1; j < traj_vip[i].size(); j++) {
      vpDisplay::displayLine(I, traj_vip[i][j - 1], traj_vip[i][j], vpColor::green, 2);
    }
  }
}

void simStepDone_callback(const std_msgs::Bool &msg){
  std::lock_guard<std::mutex> lock(s_mutex_ros);
  s_simStepDone = msg.data;
}

void simTime_callback(const std_msgs::Float32& msg)
{
  std::lock_guard<std::mutex> lock(s_mutex_ros);
  s_simTime = msg.data;
}

int main(int argc, char **argv)
{
  double opt_tagSize = 0.08;
  bool display_tag = true;
  int opt_quad_decimate = 2;
  bool opt_verbose = false;
  bool opt_plot = false;
  bool opt_adaptive_gain = false;
  bool opt_task_sequencing = false;
  double convergence_threshold_t = 0.0005, convergence_threshold_tu = vpMath::rad(0.5);

  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--tag_size" && i + 1 < argc) {
      opt_tagSize = std::stod(argv[i + 1]);
    }
    else if (std::string(argv[i]) == "--verbose") {
      opt_verbose = true;
    }
    else if (std::string(argv[i]) == "--plot") {
      opt_plot = true;
    }
    else if (std::string(argv[i]) == "--adaptive_gain") {
      opt_adaptive_gain = true;
    }
    else if (std::string(argv[i]) == "--task_sequencing") {
      opt_task_sequencing = true;
    }
    else if (std::string(argv[i]) == "--quad_decimate" && i + 1 < argc) {
      opt_quad_decimate = std::stoi(argv[i + 1]);
    }
    else if (std::string(argv[i]) == "--no-convergence-threshold") {
      convergence_threshold_t = 0.;
      convergence_threshold_tu = 0.;
    }
    else if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
      std::cout << argv[0]
          << "[--tag_size <marker size in meter; default " << opt_tagSize << ">] "
          << "[--quad_decimate <decimation; default " << opt_quad_decimate << ">] "
          << "[--adaptive_gain] "
          << "[--plot] "
          << "[--task_sequencing] "
          << "[--no-convergence-threshold] "
          << "[--verbose] "
          << "[--help] [-h]" << std::endl;;
      return EXIT_SUCCESS;
    }
  }

  try {   
    //------------------------------------------------------------------------//
    //------------------------------------------------------------------------//
    // ROS node
    ros::init(argc, argv, "visp_ros");
    ros::NodeHandlePtr n = boost::make_shared<ros::NodeHandle>();
    ros::Rate loop_rate(1000);
    ros::spinOnce();

    vpROSRobotFrankaSim robot;
    robot.setVerbose(true);
    robot.setJointStateTopic("/vrep/franka/joint_state");
    robot.seteMcTopic("/vrep/franka/eMc");
    robot.connect();

    ros::Publisher enableSyncMode_pub = n->advertise<std_msgs::Bool>("/enableSyncMode", 1);
    ros::Publisher startSimTrigger_pub = n->advertise<std_msgs::Bool>("/startSimulation", 1);
    ros::Publisher stopSimTrigger_pub = n->advertise<std_msgs::Bool>("/stopSimulation", 1);
    std_msgs::Bool trigger;
    std_msgs::Bool syncMode;
    std_msgs::Bool startStopSim;

    std::string simulationStepDone_topic_name = "/simulationStepDone";
    std::cout << "Subscribe to " << simulationStepDone_topic_name << std::endl;
    ros::Subscriber sub_simStepDone = n->subscribe(simulationStepDone_topic_name, 1, simStepDone_callback);
    std::string simulationTime_topic_name = "/simulationTime";
    std::cout << "Subscribe to " << simulationTime_topic_name << std::endl;
    ros::Subscriber sub_simulationTime = n->subscribe(simulationTime_topic_name, 1, simTime_callback);

    startStopSim.data = true;
    startSimTrigger_pub.publish(startStopSim);
    vpTime::wait(1000);
    stopSimTrigger_pub.publish(startStopSim);
    vpTime::wait(1000);
    syncMode.data = true;
    enableSyncMode_pub.publish(trigger);
    startSimTrigger_pub.publish(startStopSim);
    vpTime::wait(1000);

    if (0) {
      std::cout << "Move to modified initial position" << std::endl;
      robot.setRobotState(vpRobot::STATE_POSITION_CONTROL);
      vpColVector q;
      robot.getPosition(vpRobot::JOINT_STATE, q);
      q[0] += vpMath::rad(10); // Add 10 deg axis 1
      robot.setPosition(vpRobot::JOINT_STATE, q);
    }

    vpImage<unsigned char> I;
    vpROSGrabber g;
    g.setImageTopic("/vrep/franka/camera/image");
    g.setCameraInfoTopic("/vrep/franka/camera/camera_info");
    g.open(argc, argv);

    g.acquire(I);

    std::cout << "Image size: " << I.getWidth() << " x " << I.getHeight() << std::endl;
    vpCameraParameters cam;

    g.getCameraInfo(cam);
    std::cout << cam << std::endl;
    vpDisplayOpenCV dc(I, 10, 10, "Color image");

    vpDetectorAprilTag::vpAprilTagFamily tagFamily = vpDetectorAprilTag::TAG_36h11;
    vpDetectorAprilTag::vpPoseEstimationMethod poseEstimationMethod = vpDetectorAprilTag::HOMOGRAPHY_VIRTUAL_VS;
    vpDetectorAprilTag detector(tagFamily);
    detector.setAprilTagPoseEstimationMethod(poseEstimationMethod);
    detector.setDisplayTag(display_tag);
    detector.setAprilTagQuadDecimate(opt_quad_decimate);

    // Servo
    vpHomogeneousMatrix cdMc, cMo, oMo;

    // Desired pose used to compute the desired features
    vpHomogeneousMatrix cdMo( vpTranslationVector(0, 0.0, opt_tagSize * 3),
                              vpRotationMatrix( {1, 0, 0, 0, -1, 0, 0, 0, -1} ) );
    cdMc = cdMo * cMo.inverse();

    // Create visual features
    vpFeatureTranslation t(vpFeatureTranslation::cdMc);
    vpFeatureThetaU tu(vpFeatureThetaU::cdRc);
    t.buildFrom(cdMc);
    tu.buildFrom(cdMc);

    vpFeatureTranslation td(vpFeatureTranslation::cdMc);
    vpFeatureThetaU tud(vpFeatureThetaU::cdRc);

    vpServo task;
    // Add the visual features
    task.addFeature(t, td);
    task.addFeature(tu, tud);

    task.setServo(vpServo::EYEINHAND_CAMERA);
    task.setInteractionMatrixType(vpServo::CURRENT);

    if (opt_adaptive_gain) {
      std::cout << "Enable adptive gain" << std::endl;
      vpAdaptiveGain lambda(4, 1.2, 25); // lambda(0)=4, lambda(oo)=1.2 and lambda'(0)=25
      task.setLambda(lambda);
    }
    else {
      task.setLambda(1.2);
    }

    vpPlot *plotter = nullptr;

    if (opt_plot) {
      plotter = new vpPlot(2, static_cast<int>(250 * 2), 500, static_cast<int>(I.getWidth()) + 80, 10, "Real time curves plotter");
      plotter->setTitle(0, "Visual features error");
      plotter->setTitle(1, "Camera velocities");
      plotter->initGraph(0, 6);
      plotter->initGraph(1, 6);
      plotter->setLegend(0, 0, "error_feat_tx");
      plotter->setLegend(0, 1, "error_feat_ty");
      plotter->setLegend(0, 2, "error_feat_tz");
      plotter->setLegend(0, 3, "error_feat_theta_ux");
      plotter->setLegend(0, 4, "error_feat_theta_uy");
      plotter->setLegend(0, 5, "error_feat_theta_uz");
      plotter->setLegend(1, 0, "vc_x");
      plotter->setLegend(1, 1, "vc_y");
      plotter->setLegend(1, 2, "vc_z");
      plotter->setLegend(1, 3, "wc_x");
      plotter->setLegend(1, 4, "wc_y");
      plotter->setLegend(1, 5, "wc_z");
    }

    bool final_quit = false;
    bool has_converged = false;
    bool send_velocities = false;
    bool servo_started = false;
    std::vector<vpImagePoint> *traj_corners = nullptr; // To memorize point trajectory

    static double t_init_servo = vpTime::measureTimeMs();

    float current_time = s_simTime;
    float delta_t = 0;

    if (0) {
      // Instead of setting eMc from /vrep/franka/eMc topic, we can set its value to introduce noise for example
      vpHomogeneousMatrix eMc;
      eMc.buildFrom(0.05, -0.05, 0, 0, 0, M_PI_4);
      robot.set_eMc(eMc);
    }
    std::cout << "eMc:\n" << robot.get_eMc() << std::endl;

    vpColVector v_c(6);

    robot.setRobotState(vpRobot::STATE_VELOCITY_CONTROL);

    while (!final_quit) {
      ros::spinOnce();
      if (s_simStepDone){
        s_mutex_ros.lock();
        s_simStepDone = false;
        current_time = s_simTime;
        s_mutex_ros.unlock();

        g.acquire(I);
        vpDisplay::display(I);

        std::vector<vpHomogeneousMatrix> cMo_vec;
        detector.detect(I, opt_tagSize, cam, cMo_vec);

        {
          std::stringstream ss;
          ss << "Left click to " << (send_velocities ? "stop the robot" : "servo the robot") << ", right click to quit.";
          vpDisplay::displayText(I, 20, 20, ss.str(), vpColor::red);
        }

        // Only one tag is detected
        if (cMo_vec.size() == 1) {
          cMo = cMo_vec[0];

          static bool first_time = true;
          if (first_time) {
            // Introduce security wrt tag positionning in order to avoid PI rotation
            std::vector<vpHomogeneousMatrix> v_oMo(2), v_cdMc(2);
            v_oMo[1].buildFrom(0, 0, 0, 0, 0, M_PI);
            for (size_t i = 0; i < 2; i++) {
              v_cdMc[i] = cdMo * v_oMo[i] * cMo.inverse();
            }
            if (std::fabs(v_cdMc[0].getThetaUVector().getTheta()) < std::fabs(v_cdMc[1].getThetaUVector().getTheta())) {
              oMo = v_oMo[0];
            }
            else {
              std::cout << "Desired frame modified to avoid PI rotation of the camera" << std::endl;
              oMo = v_oMo[1];   // Introduce PI rotation
            }           
          } // end first_time

          // Update visual features
          cdMc = cdMo * oMo * cMo.inverse();
          t.buildFrom(cdMc);
          tu.buildFrom(cdMc);

          if (opt_task_sequencing) {
            if (! servo_started) {
              if (send_velocities) {
                servo_started = true;
              }
              t_init_servo = vpTime::measureTimeMs();
            }
            v_c = task.computeControlLaw((vpTime::measureTimeMs() - t_init_servo)/1000.);
          }
          else {
            v_c = task.computeControlLaw();
          }

          // Display the current and desired feature points in the image display
          // Display desired and current pose features
          vpDisplay::displayFrame(I, cdMo * oMo, cam, opt_tagSize / 1.5, vpColor::yellow, 2);
          vpDisplay::displayFrame(I, cMo,  cam, opt_tagSize / 2,   vpColor::none,   3);

          // Get tag corners
          std::vector<vpImagePoint> corners = detector.getPolygon(0);

          // Get the tag cog corresponding to the projection of the tag frame in the image
          corners.push_back(detector.getCog(0));
          // Display the trajectory of the points
          if (first_time) {
             traj_corners = new std::vector<vpImagePoint> [corners.size()];
          }

          // Display the trajectory of the points used as features
          display_point_trajectory(I, corners, traj_corners);

          if (opt_plot) {
            plotter->plot(0, current_time, task.getError());
            plotter->plot(1, current_time, v_c);
          }

          if (opt_verbose) {
            std::cout << "v_c: " << v_c.t() << std::endl;
          }

          vpTranslationVector cd_t_c = cdMc.getTranslationVector();
          vpThetaUVector cd_tu_c = cdMc.getThetaUVector();
          double error_tr = sqrt(cd_t_c.sumSquare());
          double error_tu = vpMath::deg(sqrt(cd_tu_c.sumSquare()));

          std::stringstream ss;
          ss << "error_t: " << error_tr;
          vpDisplay::displayText(I, 20, static_cast<int>(I.getWidth()) - 150, ss.str(), vpColor::red);
          ss.str("");
          ss << "error_tu: " << error_tu;
          vpDisplay::displayText(I, 40, static_cast<int>(I.getWidth()) - 150, ss.str(), vpColor::red);

          if (opt_verbose)
            std::cout << "error translation: " << error_tr << " ; error rotation: " << error_tu << std::endl;

          if (!has_converged && error_tr < convergence_threshold_t && error_tu < convergence_threshold_tu) {
           has_converged = true;
            std::cout << "Servo task has converged" << "\n";
            vpDisplay::displayText(I, 100, 20, "Servo task has converged", vpColor::red);
          }

          if (first_time) {
            first_time = false;
          }
        } // end if (cMo_vec.size() == 1)
        else {
          v_c = 0; // Stop the robot
        }

        if (!send_velocities) {
          v_c = 0; // Stop the robot
        }

        robot.setVelocity(vpRobot::CAMERA_FRAME, v_c);

        trigger.data = true;

        std::stringstream ss;
        ss << "Loop time: " << delta_t * 1000<< " ms";
        vpDisplay::displayText(I, 40, 20, ss.str(), vpColor::red);

        vpDisplay::flush(I);
        vpTime::wait(30);
      } // end if (s_simStepDone)

      vpMouseButton::vpMouseButtonType button;
      if (vpDisplay::getClick(I, button, false)) {
        switch (button) {
        case vpMouseButton::button1:
          send_velocities = !send_velocities;
          break;

        case vpMouseButton::button3:
          final_quit = true;
          v_c = 0;
          break;

        default:
          break;
        }
      }
    } //end while


    if (opt_plot && plotter != nullptr) {
      delete plotter;
      plotter = nullptr;
    }
    stopSimTrigger_pub.publish(startStopSim);

    if (!final_quit) {
      while (!final_quit) {
        g.acquire(I);
        vpDisplay::display(I);

        vpDisplay::displayText(I, 20, 20, "Click to quit the program.", vpColor::red);
        vpDisplay::displayText(I, 40, 20, "Visual servo converged.", vpColor::red);

        if (vpDisplay::getClick(I, false)) {
          final_quit = true;
        }

        vpDisplay::flush(I);
      }
    }
    if (traj_corners) {
      delete [] traj_corners;
    }
  }
  catch(const vpException &e) {
    std::cout << "ViSP exception: " << e.what() << std::endl;
    std::cout << "Stop the robot " << std::endl;
    return EXIT_FAILURE;
  }

  return 0;
}

