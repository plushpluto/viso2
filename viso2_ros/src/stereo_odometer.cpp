#include <ros/ros.h>
#include <sensor_msgs/image_encodings.h>
#include <image_geometry/stereo_camera_model.h>
#include <cv_bridge/cv_bridge.h>
#include <pcl_ros/point_cloud.h>
#include <pcl/point_types.h>

#include <viso_stereo.h>

#include "stereo_processor.h"
#include "odometer_base.h"
#include "odometry_params.h"

namespace viso2_ros
{

// some arbitrary values (0.1m linear cov. 10deg. angular cov.)
static const boost::array<double, 36> STANDARD_POSE_COVARIANCE =
{ { 0.1, 0, 0, 0, 0, 0,
    0, 0.1, 0, 0, 0, 0,
    0, 0, 0.1, 0, 0, 0,
    0, 0, 0, 0.17, 0, 0,
    0, 0, 0, 0, 0.17, 0,
    0, 0, 0, 0, 0, 0.17 } };
static const boost::array<double, 36> STANDARD_TWIST_COVARIANCE =
{ { 0.05, 0, 0, 0, 0, 0,
    0, 0.05, 0, 0, 0, 0,
    0, 0, 0.05, 0, 0, 0,
    0, 0, 0, 0.09, 0, 0,
    0, 0, 0, 0, 0.09, 0,
    0, 0, 0, 0, 0, 0.09 } };
static const boost::array<double, 36> BAD_COVARIANCE =
{ { 9999, 0, 0, 0, 0, 0,
    0, 9999, 0, 0, 0, 0,
    0, 0, 9999, 0, 0, 0,
    0, 0, 0, 9999, 0, 0,
    0, 0, 0, 0, 9999, 0,
    0, 0, 0, 0, 0, 9999 } };


class StereoOdometer : public StereoProcessor, public OdometerBase
{

private:

  boost::shared_ptr<VisualOdometryStereo> visual_odometer_;
  VisualOdometryStereo::parameters visual_odometer_params_;

  ros::Publisher point_cloud_pub_;

  bool got_lost_;

public:

  typedef pcl::PointCloud<pcl::PointXYZRGB> PointCloud;

  StereoOdometer(const std::string& transport) : StereoProcessor(transport), OdometerBase(), got_lost_(false)
  {
    // Read local parameters
    ros::NodeHandle local_nh("~");
    odometry_params::loadParams(local_nh, visual_odometer_params_);

    point_cloud_pub_ = local_nh.advertise<PointCloud>("point_cloud", 1);
  }

protected:

  void imageCallback(
      const sensor_msgs::ImageConstPtr& l_image_msg,
      const sensor_msgs::ImageConstPtr& r_image_msg,
      const sensor_msgs::CameraInfoConstPtr& l_info_msg,
      const sensor_msgs::CameraInfoConstPtr& r_info_msg)
  {
 
    bool first_run = false;
    // create odometer if not exists
    if (!visual_odometer_)
    {
      first_run = true;
      // read calibration info from camera info message
      // to fill remaining parameters
      image_geometry::StereoCameraModel model;
      model.fromCameraInfo(*l_info_msg, *r_info_msg);
      visual_odometer_params_.base = model.baseline();
      visual_odometer_params_.calib.f = model.left().fx();
      visual_odometer_params_.calib.cu = model.left().cx();
      visual_odometer_params_.calib.cv = model.left().cy();
      visual_odometer_.reset(new VisualOdometryStereo(visual_odometer_params_));
      if (l_info_msg->header.frame_id != "") setSensorFrameId(l_info_msg->header.frame_id);
      ROS_INFO_STREAM("Initialized libviso2 stereo odometry "
                      "with the following parameters:" << std::endl << 
                      visual_odometer_params_);
    }

    // convert images if necessary
    uint8_t *l_image_data, *r_image_data;
    int l_step, r_step;
    cv_bridge::CvImageConstPtr l_cv_ptr, r_cv_ptr;
    if (l_image_msg->encoding == sensor_msgs::image_encodings::MONO8)
    {
      l_image_data = const_cast<uint8_t*>(&(l_image_msg->data[0]));
      l_step = l_image_msg->step;
    }
    else
    {
      l_cv_ptr = cv_bridge::toCvShare(l_image_msg, sensor_msgs::image_encodings::MONO8);
      l_image_data = l_cv_ptr->image.data;
      l_step = l_cv_ptr->image.step[0];
    }
    if (r_image_msg->encoding == sensor_msgs::image_encodings::MONO8)
    {
      r_image_data = const_cast<uint8_t*>(&(r_image_msg->data[0]));
      r_step = r_image_msg->step;
    }
    else
    {
      r_cv_ptr = cv_bridge::toCvShare(r_image_msg, sensor_msgs::image_encodings::MONO8);
      r_image_data = r_cv_ptr->image.data;
      r_step = r_cv_ptr->image.step[0];
    }

    ROS_ASSERT(l_step == r_step);
    ROS_ASSERT(l_image_msg->width == r_image_msg->width);
    ROS_ASSERT(l_image_msg->height == r_image_msg->height);

    // run the odometer
    int32_t dims[] = {l_image_msg->width, l_image_msg->height, l_step};
    // on first run or when odometer got lost, only feed the odometer with image pair without
    // retrieving data
    if (first_run || got_lost_)
    {
      visual_odometer_->process(l_image_data, r_image_data, dims);
      got_lost_ = false;
    }
    else
    {
      if (visual_odometer_->process(l_image_data, r_image_data, dims))
      {
        Matrix camera_motion = Matrix::inv(visual_odometer_->getMotion());
        ROS_DEBUG("Found %i matches with %i inliers.", 
                  visual_odometer_->getNumberOfMatches(),
                  visual_odometer_->getNumberOfInliers());
        ROS_DEBUG_STREAM("libviso2 returned the following motion:\n" << camera_motion);

        btMatrix3x3 rot_mat(
          camera_motion.val[0][0], camera_motion.val[0][1], camera_motion.val[0][2],
          camera_motion.val[1][0], camera_motion.val[1][1], camera_motion.val[1][2],
          camera_motion.val[2][0], camera_motion.val[2][1], camera_motion.val[2][2]);
        btVector3 t(camera_motion.val[0][3], camera_motion.val[1][3], camera_motion.val[2][3]);
        tf::Transform delta_transform(rot_mat, t);

        setPoseCovariance(STANDARD_POSE_COVARIANCE);
        setTwistCovariance(STANDARD_TWIST_COVARIANCE);

        integrateAndPublish(delta_transform, l_image_msg->header.stamp);

        if (point_cloud_pub_.getNumSubscribers() > 0)
        {
          computeAndPublishPointCloud(l_info_msg, l_image_msg, r_info_msg, visual_odometer_->getMatches(), visual_odometer_->getInlierIndices());
        }
      }
      else
      {
        setPoseCovariance(BAD_COVARIANCE);
        setTwistCovariance(BAD_COVARIANCE);
        tf::Transform delta_transform;
        delta_transform.setIdentity();
        integrateAndPublish(delta_transform, l_image_msg->header.stamp);

        ROS_DEBUG("Call to VisualOdometryStereo::process() failed.");
        ROS_WARN_THROTTLE(1.0, "Visual Odometer got lost!");
        got_lost_ = true;
      }
    }
  }

  void computeAndPublishPointCloud(
      const sensor_msgs::CameraInfoConstPtr& l_info_msg,
      const sensor_msgs::ImageConstPtr& l_image_msg,
      const sensor_msgs::CameraInfoConstPtr& r_info_msg, 
      const std::vector<Matcher::p_match>& matches,
      const std::vector<int32_t>& inlier_indices)
  {
    try
    {
      cv_bridge::CvImageConstPtr cv_ptr;
      cv_ptr = cv_bridge::toCvShare(l_image_msg, sensor_msgs::image_encodings::RGB8);
      // read calibration info from camera info message
      image_geometry::StereoCameraModel model;
      model.fromCameraInfo(*l_info_msg, *r_info_msg);
      PointCloud::Ptr point_cloud(new PointCloud());
      point_cloud->header.frame_id = l_info_msg->header.frame_id;
      point_cloud->header.stamp = l_info_msg->header.stamp;
      point_cloud->width = 1;
      point_cloud->height = inlier_indices.size();
      point_cloud->points.resize(inlier_indices.size());

      for (size_t i = 0; i < inlier_indices.size(); ++i)
      {
        const Matcher::p_match& match = matches[inlier_indices[i]];
        cv::Point2d left_uv;
        left_uv.x = match.u1c;
        left_uv.y = match.v1c;
        cv::Point3d point;
        double disparity = match.u1c - match.u2c;
        model.projectDisparityTo3d(left_uv, disparity, point);
        point_cloud->points[i].x = point.x;
        point_cloud->points[i].y = point.y;
        point_cloud->points[i].z = point.z;
        cv::Vec3b colors = cv_ptr->image.at<cv::Vec3b>(left_uv.y,left_uv.x);
        point_cloud->points[i].r = colors[0];
        point_cloud->points[i].g = colors[1];
        point_cloud->points[i].b = colors[2];
      }
      ROS_DEBUG("Publishing point cloud with %zu points.", point_cloud->size());
      point_cloud_pub_.publish(point_cloud);
    }
    catch (cv_bridge::Exception& e)
    {
      ROS_ERROR("cv_bridge exception: %s", e.what());
    }
  }
};

} // end of namespace


int main(int argc, char **argv)
{
  ros::init(argc, argv, "stereo_odometer");
  if (ros::names::remap("stereo") == "stereo") {
    ROS_WARN("'stereo' has not been remapped! Example command-line usage:\n"
             "\t$ rosrun viso2_ros stereo_odometer stereo:=narrow_stereo image:=image_rect");
  }
  if (ros::names::remap("image").find("rect") == std::string::npos) {
    ROS_WARN("stereo_odometer needs rectified input images. The used image "
             "topic is '%s'. Are you sure the images are rectified?",
             ros::names::remap("image").c_str());
  }

  std::string transport = argc > 1 ? argv[1] : "raw";
  viso2_ros::StereoOdometer odometer(transport);
  
  ros::spin();
  return 0;
}

