/****************************************************************
 **                                                            **
 **  Copyright(C) 2016 Quanergy Systems. All Rights Reserved.  **
 **  Contact: http://www.quanergy.com                          **
 **                                                            **
 ****************************************************************/

#include <quanergy/parsers/data_packet_parser_m8.h>

namespace quanergy
{
  namespace client
  {

    DataPacketParserM8::DataPacketParserM8()
      : DataPacketParser()
      , packet_counter_(0)
      , cloud_counter_(0)
      , last_azimuth_(65000)
      , current_cloud_(new PointCloudHVDIR())
      , worker_cloud_(new PointCloudHVDIR())
      , horizontal_angle_lookup_table_(M8_NUM_ROT_ANGLES+1)
    {
      for (std::uint32_t i = 0; i <= M8_NUM_ROT_ANGLES; i++)
      {
        // Shift by half the rot angles to keep the number positive when wrapping.
        std::uint32_t j = (i + M8_NUM_ROT_ANGLES/2) % M8_NUM_ROT_ANGLES;

        // normalized
        double n = static_cast<double>(j) / static_cast<double>(M8_NUM_ROT_ANGLES);

        double rad = n * M_PI * 2.0 - M_PI;

        horizontal_angle_lookup_table_[i] = rad;
      }

      const double* angle_in_radians = M8_VERTICAL_ANGLES;
      for (std::uint32_t i = 0; i < M8_NUM_LASERS; ++i, ++angle_in_radians)
      {
        vertical_angle_lookup_table_[i] = *angle_in_radians;
      }
    }

    void DataPacketParserM8::setReturnSelection(ReturnSelection return_selection)
    {
      return_selection_ = return_selection;
    }

    void DataPacketParserM8::setCloudSizeLimits(std::int32_t szmin, std::int32_t szmax)
    {
      if(szmin > 0)
        minimum_cloud_size_ = std::max(1,szmin);
      if(szmax > 0)
        maximum_cloud_size_ = std::max(minimum_cloud_size_,szmax);
    }

    bool DataPacketParserM8::parse(const M8DataPacket& data_packet, PointCloudHVDIRPtr& result)
    {
      bool ret = false;
      if (data_packet.status != 0)
      {
        std::cerr << "Sensor status nonzero: " << data_packet.status;
        if (data_packet.status == 1)
          throw FirmwareVersionMismatchError();
        return ret; // don't process if sensor in error
      }

      // this time is used for the cloud stamp which is a 64 bit integer in units of microseconds
      std::uint64_t time;
      if (data_packet.version <= 3)
      {
        // early versions of API put 10 ns increments in this field
        time = data_packet.seconds * 1E6 + data_packet.nanoseconds * 1E-2;
      }
      else
        time = data_packet.seconds * 1E6 + data_packet.nanoseconds * 1E-3;

      ++packet_counter_;

      // get spin direction
      int direction = 0;
      if (data_packet.data[0].position - data_packet.data[M8_FIRING_PER_PKT-1].position > 0)
        direction = (data_packet.data[0].position - data_packet.data[M8_FIRING_PER_PKT-1].position > 4000) ? 1 : -1;
      else
        direction = (data_packet.data[M8_FIRING_PER_PKT-1].position - data_packet.data[0].position > 4000) ? -1 : 1;

      bool cloudfull = (current_cloud_->size() >= maximum_cloud_size_);

      // for each firing
      for (int i = 0; i < M8_FIRING_PER_PKT; ++i)
      {
        const M8FiringData &data = data_packet.data[i];

        // calculate the angle in degrees
        double azimuth_angle = (static_cast<double> ((data.position+(M8_NUM_ROT_ANGLES/2))%M8_NUM_ROT_ANGLES) / (M8_NUM_ROT_ANGLES) * 360.0) - 180.;
        // check for wrap which indicates completion of a scan
        if (direction * azimuth_angle < direction * last_azimuth_)
        {
          if (current_cloud_->size () > minimum_cloud_size_)
          {
            if(cloudfull)
            {
              std::cout << "Warning: Maximum cloud size limit of ("
                  << maximum_cloud_size_ << ") exceeded" << std::endl;
            }

            current_cloud_->header.stamp = time;
            current_cloud_->header.seq = cloud_counter_;
            current_cloud_->header.frame_id = frame_id_;

            // can't organize if we kept all points
            if (return_selection_ != ReturnSelection::ALL)
            {
              organizeCloud(current_cloud_, worker_cloud_);
            }

            ++cloud_counter_;

            // fire the signal that we have a new cloud
            result = current_cloud_;
            ret = true;
          }
          else if(current_cloud_->size() > 0)
          {
            std::cout << "Warning: Minimum cloud size limit of (" << minimum_cloud_size_
                << ") not reached (" << current_cloud_->size() << ")" << std::endl;
          }

          // start a new cloud
          current_cloud_.reset(new PointCloudHVDIR());
          // at first we assume it is dense
          current_cloud_->is_dense = true;
          cloudfull = false;
        }

        if(cloudfull)
          continue;

        double const horizontal_angle = horizontal_angle_lookup_table_[data.position];

        double distance_scaling = 0.01f;
        if (data_packet.version > 4)
        {
          distance_scaling = 0.01f;
        }

        for (int j = 0; j < M8_NUM_LASERS; j++)
        {
          // output point
          PointCloudHVDIR::PointType hvdir;

          double const vertical_angle = vertical_angle_lookup_table_[j];

          hvdir.h = horizontal_angle;
          hvdir.v = vertical_angle;
          hvdir.ring = j;

          if (return_selection_ == ReturnSelection::ALL)
          {
            // for the all case, we won't keep NaN points and we'll compare
            // distances to illiminate duplicates
            // index 0 (max return) could equal index 1 (first) and/or index 2 (last)
            hvdir.intensity = data.returns_intensities[0][j];
            std::uint32_t d = data.returns_distances[0][j];
            if (d != 0)
            {
              hvdir.d = static_cast<float>(d) * distance_scaling; // convert range to meters
              // add the point to the current scan
              current_cloud_->push_back(hvdir);
            }

            if (data.returns_distances[1][j] != 0 && data.returns_distances[1][j] != d)
            {
              hvdir.intensity = data.returns_intensities[0][j];
              hvdir.d = static_cast<float>(data.returns_distances[1][j]) * distance_scaling; // convert range to meters
              // add the point to the current scan
              current_cloud_->push_back(hvdir);
            }

            if (data.returns_distances[2][j] != 0 && data.returns_distances[2][j] != d)
            {
              hvdir.intensity = data.returns_intensities[0][j];
              hvdir.d = static_cast<float>(data.returns_distances[2][j]) * distance_scaling; // convert range to meters
              // add the point to the current scan
              current_cloud_->push_back(hvdir);
            }
          }
          else
          {
            for (int i = 0; i < 3; ++i)
            {
              if (static_cast<int>(return_selection_) == i)
              {
                hvdir.intensity = data.returns_intensities[i][j];

                if (data.returns_distances[i][j] == 0)
                {
                  hvdir.d = std::numeric_limits<float>::quiet_NaN();
                  // if the range is NaN, the cloud is not dense
                  current_cloud_->is_dense = false;
                }
                else
                {
                  hvdir.d = static_cast<float>(data.returns_distances[i][j]) * distance_scaling; // convert range to meters
                }

                // add the point to the current scan
                current_cloud_->push_back(hvdir);
              }
            }
          }
        }

        last_azimuth_ = azimuth_angle;
      }

      return ret;
    }

    void DataPacketParserM8::organizeCloud(PointCloudHVDIRPtr & current_pc,
                                           PointCloudHVDIRPtr & temp_pc)
    {
      // transpose the cloud
      temp_pc->clear();

      temp_pc->header.stamp = current_pc->header.stamp;
      temp_pc->header.seq = current_pc->header.seq;
      temp_pc->header.frame_id = current_pc->header.frame_id;

      // reserve space
      temp_pc->reserve(current_pc->size());

      unsigned int temp_index;
      unsigned int width = current_pc->size () / M8_NUM_LASERS; // CONSTANT FOR NUM BEAMS

      // iterate through each ring from top down
      for (int i = M8_NUM_LASERS - 1; i >= 0; --i)
      {
        // iterate through width in collect order
        for (unsigned int j = 0; j < width; ++j)
        {
          // original data is in collect order and laser order
          temp_index = j * M8_NUM_LASERS + i;

          temp_pc->push_back(current_pc->points[temp_index]);
        }
      }

      current_pc.swap(temp_pc);

      current_pc->height = M8_NUM_LASERS;
      current_pc->width  = width;
    }

  } // namespace client

} // namespace quanergy
