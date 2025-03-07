/**
 * @copyright 2020, 2021 Istituto Italiano di Tecnologia (IIT). This software may be modified and
 * distributed under the terms of the BSD-3-Clause license.
 */

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>

#include <BipedalLocomotion/ParametersHandler/IParametersHandler.h>
#include <BipedalLocomotion/ParametersHandler/YarpImplementation.h>
#include <BipedalLocomotion/System/Clock.h>
#include <BipedalLocomotion/System/YarpClock.h>
#include <BipedalLocomotion/TextLogging/Logger.h>
#include <BipedalLocomotion/TextLogging/LoggerBuilder.h>
#include <BipedalLocomotion/TextLogging/YarpLogger.h>
#include <BipedalLocomotion/YarpRobotLoggerDevice.h>
#include <BipedalLocomotion/YarpTextLoggingUtilities.h>
#include <BipedalLocomotion/YarpUtilities/Helper.h>
#include <BipedalLocomotion/YarpUtilities/VectorsCollection.h>

#include <yarp/eigen/Eigen.h>
#include <yarp/os/BufferedPort.h>
#include <yarp/profiler/NetworkProfiler.h>

#include <robometry/BufferConfig.h>
#include <robometry/BufferManager.h>

#include <process.hpp>

using namespace BipedalLocomotion::YarpUtilities;
using namespace BipedalLocomotion::ParametersHandler;
using namespace BipedalLocomotion::RobotInterface;
using namespace BipedalLocomotion;

VISITABLE_STRUCT(TextLoggingEntry,
                 level,
                 text,
                 filename,
                 line,
                 function,
                 hostname,
                 cmd,
                 args,
                 pid,
                 thread_id,
                 component,
                 id,
                 systemtime,
                 networktime,
                 externaltime,
                 backtrace,
                 yarprun_timestamp,
                 local_timestamp);

void findAndReplaceAll(std::string& data, std::string toSearch, std::string replaceStr)
{
    // Get the first occurrence
    size_t pos = data.find(toSearch);
    // Repeat till end is reached
    while (pos != std::string::npos)
    {
        // Replace this occurrence of Sub String
        data.replace(pos, toSearch.size(), replaceStr);
        // Get the next occurrence from the current position
        pos = data.find(toSearch, pos + replaceStr.size());
    }
}

YarpRobotLoggerDevice::YarpRobotLoggerDevice(double period,
                                             yarp::os::ShouldUseSystemClock useSystemClock)
    : yarp::os::PeriodicThread(period, useSystemClock)
{
    // Use the yarp clock in blf
    BipedalLocomotion::System::ClockBuilder::setFactory(
        std::make_shared<BipedalLocomotion::System::YarpClockFactory>());

    // the logging message are streamed using yarp
    BipedalLocomotion::TextLogging::LoggerBuilder::setFactory(
        std::make_shared<BipedalLocomotion::TextLogging::YarpLoggerFactory>());
}

YarpRobotLoggerDevice::YarpRobotLoggerDevice()
    : yarp::os::PeriodicThread(0.01, yarp::os::ShouldUseSystemClock::No)
{
    // Use the yarp clock in blf
    BipedalLocomotion::System::ClockBuilder::setFactory(
        std::make_shared<BipedalLocomotion::System::YarpClockFactory>());

    // the logging message are streamed using yarp
    BipedalLocomotion::TextLogging::LoggerBuilder::setFactory(
        std::make_shared<BipedalLocomotion::TextLogging::YarpLoggerFactory>());
}

YarpRobotLoggerDevice::~YarpRobotLoggerDevice() = default;

bool YarpRobotLoggerDevice::open(yarp::os::Searchable& config)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::open]";
    auto params = std::make_shared<ParametersHandler::YarpImplementation>(config);

    double devicePeriod{0.01};
    if (params->getParameter("sampling_period_in_s", devicePeriod))
    {
        this->setPeriod(devicePeriod);
    }

    if (!params->getParameter("text_logging_subnames", m_textLoggingSubnames))
    {
        log()->info("{} Unable to get the 'text_logging_subnames' parameter for the telemetry. All "
                    "the ports related to the text logging will be considered.",
                    logPrefix);
    }

    if (!params->getParameter("code_status_cmd_prefixes", m_codeStatusCmdPrefixes))
    {
        log()->info("{} Unable to get the 'code_status_cmd_prefixes' parameter. No prefix will be "
                    "added to commands.",
                    logPrefix);
    }

    if (!this->setupRobotSensorBridge(params->getGroup("RobotSensorBridge")))
    {
        return false;
    }

    if (this->setupRobotCameraBridge(params->getGroup("RobotCameraBridge")))
    {
        auto populateCamerasData
            = [logPrefix, params, this](const std::string& fpsParamName,
                                        const std::vector<std::string>& cameraNames) -> bool {
            std::vector<int> fps, depthScale;
            std::vector<std::string> rgbSaveMode, depthSaveMode;
            if (!params->getParameter(fpsParamName, fps))
            {
                log()->error("{} Unable to find the parameter named: {}.", logPrefix, fpsParamName);
                return false;
            }

            // if the camera is an rgbd camera then user should provide the depth scale
            if ((&cameraNames) == (&m_cameraBridge->getMetaData().sensorsList.rgbdCamerasList))
            {
                if (!params->getParameter("rgbd_cameras_depth_scale", depthScale))
                {
                    log()->error("{} Unable to find the parameter named: "
                                 "'rgbd_cameras_depth_scale'.",
                                 logPrefix);
                    return false;
                }

                if (!params->getParameter("rgbd_cameras_rgb_save_mode", rgbSaveMode))
                {
                    log()->error("{} Unable to find the parameter named: "
                                 "'rgb_cameras_rgb_save_mode.",
                                 logPrefix);
                    return false;
                }

                if (!params->getParameter("rgbd_cameras_depth_save_mode", depthSaveMode))
                {
                    log()->error("{} Unable to find the parameter named: "
                                 "'rgbd_cameras_depth_save_mode.",
                                 logPrefix);
                    return false;
                }

                if (fps.size() != depthScale.size() || (fps.size() != rgbSaveMode.size())
                    || (fps.size() != depthSaveMode.size()))
                {
                    log()->error("{} Mismatch between the vector containing the size of the vector "
                                 "provided from configuration"
                                 "Number of cameras: {}. Size of the FPS vector {}. Size of the "
                                 "depth scale vector {}."
                                 "Size of 'rgb_cameras_rgb_save_mode' {}. Size of "
                                 "'rgb_cameras_depth_save_mode': {}",
                                 logPrefix,
                                 cameraNames.size(),
                                 fps.size(),
                                 depthScale.size(),
                                 rgbSaveMode.size(),
                                 depthSaveMode.size());
                    return false;
                }
            } else
            {
                if (!params->getParameter("rgb_cameras_rgb_save_mode", rgbSaveMode))
                {
                    log()->error("{} Unable to find the parameter named: "
                                 "'rgb_cameras_rgb_save_mode.",
                                 logPrefix);
                    return false;
                }
            }

            if ((fps.size() != rgbSaveMode.size()))
            {
                log()->error("{} Mismatch between the vector containing the size of the vector "
                             "provided from configuration"
                             "Number of cameras: {}. Size of the FPS vector {}."
                             "Size of 'rgb_cameras_rgb_save_mode' {}.",
                             logPrefix,
                             cameraNames.size(),
                             fps.size(),
                             rgbSaveMode.size());
                return false;
            }

            if (fps.size() != cameraNames.size())
            {
                log()->error("{} Mismatch between the number of cameras and the vector containing "
                             "the FPS. Number of cameras: {}. Size of the FPS vector {}.",
                             logPrefix,
                             cameraNames.size(),
                             fps.size());
                return false;
            }

            auto createImageSaver
                = [logPrefix](
                      const std::string& saveMode) -> std::shared_ptr<VideoWriter::ImageSaver> {
                auto saver = std::make_shared<VideoWriter::ImageSaver>();
                if (saveMode == "frame")
                {
                    saver->saveMode = VideoWriter::SaveMode::Frame;
                } else if (saveMode == "video")
                {
                    saver->saveMode = VideoWriter::SaveMode::Video;
                } else
                {
                    log()->error("{} The save mode associated to the one of the camera is neither "
                                 "'frame' nor 'video'. Provided: {}",
                                 logPrefix,
                                 saveMode);
                    return nullptr;
                }
                return saver;
            };

            bool ok = true;
            for (unsigned int i = 0; i < fps.size(); i++)
            {
                if (fps[i] <= 0)
                {
                    log()->error("{} The FPS associated to the camera {} is negative or equal to "
                                 "zero.",
                                 logPrefix,
                                 i);
                    return false;
                }

                // get the desired fps for each camera
                m_videoWriters[cameraNames[i]].fps = fps[i];

                // this means that the list of cameras are rgb camera
                if ((&cameraNames) == (&m_cameraBridge->getMetaData().sensorsList.rgbCamerasList))
                {
                    m_videoWriters[cameraNames[i]].rgb = createImageSaver(rgbSaveMode[i]);
                    ok = ok && m_videoWriters[cameraNames[i]].rgb != nullptr;
                }
                // this means that the list of cameras are rgbd camera
                else
                {
                    if (depthSaveMode[i] == "video")
                    {
                        log()->warn("{} The depth stream of the rgbd camera {} will be saved as a "
                                    "grayscale 8bit video. We suggest to save it as a set of "
                                    "frames.",
                                    logPrefix,
                                    i);
                    }
                    m_videoWriters[cameraNames[i]].rgb = createImageSaver(rgbSaveMode[i]);
                    ok = ok && m_videoWriters[cameraNames[i]].rgb != nullptr;
                    m_videoWriters[cameraNames[i]].depth = createImageSaver(depthSaveMode[i]);
                    ok = ok && m_videoWriters[cameraNames[i]].depth != nullptr;
                    m_videoWriters[cameraNames[i]].depthScale = depthScale[i];
                }
            }

            return ok;
        };

        // get the metadata for rgb camera
        if (m_cameraBridge->getMetaData().bridgeOptions.isRGBCameraEnabled)
        {
            if (!populateCamerasData("rgb_cameras_fps",
                                     m_cameraBridge->getMetaData().sensorsList.rgbCamerasList))
            {
                log()->error("{} Unable to populate the camera fps for RGB cameras.", logPrefix);
                return false;
            }
        }

        // Currently the logger supports only rgb cameras
        if (m_cameraBridge->getMetaData().bridgeOptions.isRGBDCameraEnabled)
        {
            if (!populateCamerasData("rgbd_cameras_fps",
                                     m_cameraBridge->getMetaData().sensorsList.rgbdCamerasList))
            {
                log()->error("{} Unable to populate the camera fps for RGBD cameras.", logPrefix);
                return false;
            }
        }

        // get the video codec in case rgb or rgbd camera are enabled
        if (m_cameraBridge->getMetaData().bridgeOptions.isRGBDCameraEnabled
            || m_cameraBridge->getMetaData().bridgeOptions.isRGBCameraEnabled)
        {
            if (!params->getParameter("video_codec_code", m_videoCodecCode))
            {
                constexpr auto fourccCodecUrl = "https://abcavi.kibi.ru/fourcc.php";
                log()->info("{} The parameter 'video_codec_code' is not provided. The default one "
                            "will be used {}. You can find the list of supported parameters at: "
                            "{}.",
                            logPrefix,
                            m_videoCodecCode,
                            fourccCodecUrl);
            } else if (m_videoCodecCode.size() != 4)
            {
                constexpr auto fourccCodecUrl = "https://abcavi.kibi.ru/fourcc.php";
                log()->error("{} The parameter 'video_codec_code' must be a string with 4 "
                             "characters. You can find the list of supported parameters at: {}.",
                             logPrefix,
                             fourccCodecUrl);
                return false;
            }
        }
    } else
    {
        log()->info("{} The video will not be recorded", logPrefix);
    }

    if (!this->setupTelemetry(params->getGroup("Telemetry"), devicePeriod))
    {
        return false;
    }

    if (!this->setupExogenousInputs(params->getGroup("ExogenousSignals")))
    {
        return false;
    }

    return true;
}

bool YarpRobotLoggerDevice::setupExogenousInputs(
    std::weak_ptr<const ParametersHandler::IParametersHandler> params)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::setupExogenousInputs]";

    auto ptr = params.lock();
    if (ptr == nullptr)
    {
        log()->info("{} No exogenous input will be logged.", logPrefix);
        return true;
    }

    std::vector<std::string> inputs;
    if (!ptr->getParameter("vectors_collection_exogenous_inputs", inputs))
    {
        log()->error("{} Unable to get the exogenous inputs.", logPrefix);
        return false;
    }

    for (const auto& input : inputs)
    {
        auto group = ptr->getGroup(input).lock();
        std::string local, signalName, remote, carrier;
        if (group == nullptr || !group->getParameter("local", local)
            || !group->getParameter("remote", remote) || !group->getParameter("carrier", carrier)
            || !group->getParameter("signal_name", signalName))
        {
            log()->error("{} Unable to get the parameters related to the input: {}.",
                         logPrefix,
                         input);
            return false;
        }

        m_vectorsCollectionSignals[remote].signalName = signalName;
        m_vectorsCollectionSignals[remote].remote = remote;
        m_vectorsCollectionSignals[remote].local = local;
        m_vectorsCollectionSignals[remote].carrier = carrier;

        if (!m_vectorsCollectionSignals[remote].port.open(m_vectorsCollectionSignals[remote].local))
        {
            log()->error("{} Unable to open the port named: {}.",
                         logPrefix,
                         m_vectorsCollectionSignals[remote].local);
            return false;
        }
    }

    if (!ptr->getParameter("vectors_exogenous_inputs", inputs))
    {
        log()->error("{} Unable to get the exogenous inputs.", logPrefix);
        return false;
    }

    for (const auto& input : inputs)
    {
        auto group = ptr->getGroup(input).lock();
        std::string local, signalName, remote, carrier;
        if (group == nullptr || !group->getParameter("local", local)
            || !group->getParameter("remote", remote) || !group->getParameter("carrier", carrier)
            || !group->getParameter("signal_name", signalName))
        {
            log()->error("{} Unable to get the parameters related to the input: {}.",
                         logPrefix,
                         input);
            return false;
        }

        m_vectorSignals[remote].signalName = signalName;
        m_vectorSignals[remote].remote = remote;
        m_vectorSignals[remote].local = local;
        m_vectorSignals[remote].carrier = carrier;

        if (!m_vectorSignals[remote].port.open(m_vectorSignals[remote].local))
        {
            log()->error("{} Unable to open the port named: {}.",
                         logPrefix,
                         m_vectorSignals[remote].local);
            return false;
        }
    }

    return true;
}

bool YarpRobotLoggerDevice::setupTelemetry(
    std::weak_ptr<const ParametersHandler::IParametersHandler> params, const double& devicePeriod)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::setupTelemetry]";

    auto ptr = params.lock();
    if (ptr == nullptr)
    {
        log()->error("{} The parameters handler is not valid.", logPrefix);
        return false;
    }

    robometry::BufferConfig config;
    char* tmp = std::getenv("YARP_ROBOT_NAME");
    // if the variable does not exist it points to NULL
    if (tmp != NULL)
    {
        config.yarp_robot_name = tmp;
    }
    config.filename = "robot_logger_device";
    config.auto_save = true;
    config.save_periodically = true;
    config.file_indexing = "%Y_%m_%d_%H_%M_%S";
    config.mat_file_version = matioCpp::FileVersion::MAT7_3;

    if (!ptr->getParameter("save_period", config.save_period))
    {
        log()->error("{} Unable to get the 'save_period' parameter for the telemetry.", logPrefix);
        return false;
    }

    // the telemetry will flush the content of its storage every config.save_period
    // and this device runs every devicePeriod
    // so the size of the telemetry buffer must be at least config.save_period / devicePeriod
    // to be sure we are not going to lose data the buffer will be 10% longer
    constexpr double percentage = 0.1;
    config.n_samples = static_cast<int>(std::ceil((1 + percentage) //
                                                  * (config.save_period / devicePeriod)));

    return m_bufferManager.configure(config);
}

bool YarpRobotLoggerDevice::setupRobotSensorBridge(
    std::weak_ptr<const ParametersHandler::IParametersHandler> params)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::setupRobotSensorBridge]";

    auto ptr = params.lock();
    if (ptr == nullptr)
    {
        log()->error("{} The parameters handler is not valid.", logPrefix);
        return false;
    }

    m_robotSensorBridge = std::make_unique<YarpSensorBridge>();
    if (!m_robotSensorBridge->initialize(ptr))
    {
        log()->error("{} Unable to configure the 'SensorBridge'", logPrefix);
        return false;
    }

    // Get additional flags required by the device
    if (!ptr->getParameter("stream_joint_states", m_streamJointStates))
    {
        log()->info("{} The 'stream_joint_states' parameter is not found. The joint states is not "
                    "logged",
                    logPrefix);
    }

    if (!ptr->getParameter("stream_motor_states", m_streamMotorStates))
    {
        log()->info("{} The 'stream_motor_states' parameter is not found. The motor states is not "
                    "logged",
                    logPrefix);
    }

    if (!ptr->getParameter("stream_motor_PWM", m_streamMotorPWM))
    {
        log()->info("{} The 'stream_motor_PWM' parameter is not found. The motor PWM is not logged",
                    logPrefix);
    }

    if (!ptr->getParameter("stream_pids", m_streamPIDs))
    {
        log()->info("{} The 'stream_pids' parameter is not found. The motor pid values are not "
                    "logged",
                    logPrefix);
    }

    if (!ptr->getParameter("stream_inertials", m_streamInertials))
    {
        log()->info("{} The 'stream_inertials' parameter is not found. The IMU values are not "
                    "logged",
                    logPrefix);
    }

    if (!ptr->getParameter("stream_cartesian_wrenches", m_streamCartesianWrenches))
    {
        log()->info("{} The 'stream_cartesian_wrenches' parameter is not found. The cartesian "
                    "wrench values are not "
                    "logged",
                    logPrefix);
    }

    if (!ptr->getParameter("stream_forcetorque_sensors", m_streamFTSensors))
    {
        log()->info("{} The 'stream_forcetorque_sensors' parameter is not found. The FT values are "
                    "not "
                    "logged",
                    logPrefix);
    }

    if (!ptr->getParameter("stream_temperatures", m_streamTemperatureSensors))
    {
        log()->info("{} The 'stream_temperatures' parameter is not found. The temperature sensor "
                    "values are not "
                    "logged",
                    logPrefix);
    }

    return true;
}

bool YarpRobotLoggerDevice::setupRobotCameraBridge(
    std::weak_ptr<const ParametersHandler::IParametersHandler> params)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::setupRobotCameraBridge]";

    auto ptr = params.lock();
    if (ptr == nullptr)
    {
        log()->error("{} The parameters handler is not valid.", logPrefix);
        return false;
    }

    m_cameraBridge = std::make_unique<YarpCameraBridge>();
    if (!m_cameraBridge->initialize(ptr))
    {
        log()->error("{} Unable to configure the 'Camera bridge'", logPrefix);
        return false;
    }

    return true;
}

bool YarpRobotLoggerDevice::attachAll(const yarp::dev::PolyDriverList& poly)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::attachAll]";

    if (!m_robotSensorBridge->setDriversList(poly))
    {
        log()->error("{} Could not attach drivers list to sensor bridge.", logPrefix);
        return false;
    }

    // The user can avoid to record the camera
    if (m_cameraBridge != nullptr)
    {
        if (!m_cameraBridge->setDriversList(poly))
        {
            log()->error("{} Could not attach drivers list to camera bridge.", logPrefix);
            return false;
        }
    }

    // TODO this should be removed
    // this sleep is required since the sensor bridge could be not ready
    using namespace std::chrono_literals;
    BipedalLocomotion::clock().sleepFor(2000ms);

    std::vector<std::string> joints;
    if (!m_robotSensorBridge->getJointsList(joints))
    {
        log()->error("{} Could not get the joints list.", logPrefix);
        return false;
    }

    const unsigned dofs = joints.size();
    m_bufferManager.setDescriptionList(joints);

    bool ok = true;

    // prepare the telemetry
    if (m_streamJointStates)
    {
        ok = ok && m_bufferManager.addChannel({"joints_state::positions", {dofs, 1}, joints});
        ok = ok && m_bufferManager.addChannel({"joints_state::velocities", {dofs, 1}, joints});
        ok = ok && m_bufferManager.addChannel({"joints_state::accelerations", {dofs, 1}, joints});
        ok = ok && m_bufferManager.addChannel({"joints_state::torques", {dofs, 1}, joints});
    }
    if (m_streamMotorStates)
    {
        ok = ok && m_bufferManager.addChannel({"motors_state::positions", {dofs, 1}, joints});
        ok = ok && m_bufferManager.addChannel({"motors_state::velocities", {dofs, 1}, joints});
        ok = ok && m_bufferManager.addChannel({"motors_state::accelerations", {dofs, 1}, joints});
        ok = ok && m_bufferManager.addChannel({"motors_state::currents", {dofs, 1}, joints});
    }

    if (m_streamMotorPWM)
    {
        ok = ok && m_bufferManager.addChannel({"motors_state::PWM", {dofs, 1}, joints});
    }

    if (m_streamPIDs)
    {
        ok = ok && m_bufferManager.addChannel({"PIDs", {dofs, 1}, joints});
    }

    if (m_streamFTSensors)
    {
        for (const auto& sensorName : m_robotSensorBridge->getSixAxisForceTorqueSensorsList())
        {
            ok = ok
                 && m_bufferManager.addChannel({"FTs::" + sensorName,
                                                {6, 1}, //
                                                {"f_x", "f_y", "f_z", "mu_x", "mu_y", "mu_z"}});
        }
    }

    if (m_streamInertials)
    {
        for (const auto& sensorName : m_robotSensorBridge->getGyroscopesList())
        {
            ok = ok
                 && m_bufferManager.addChannel({"gyros::" + sensorName,
                                                {3, 1}, //
                                                {"omega_x", "omega_y", "omega_z"}});
        }

        for (const auto& sensorName : m_robotSensorBridge->getLinearAccelerometersList())
        {
            ok = ok
                 && m_bufferManager.addChannel({"accelerometers::" + sensorName,
                                                {3, 1}, //
                                                {"a_x", "a_y", "a_z"}});
        }

        for (const auto& sensorName : m_robotSensorBridge->getOrientationSensorsList())
        {
            ok = ok
                 && m_bufferManager.addChannel({"orientations::" + sensorName,
                                                {3, 1}, //
                                                {"r", "p", "y"}});
        }

        for (const auto& sensorName : m_robotSensorBridge->getMagnetometersList())
        {
            ok = ok
                 && m_bufferManager.addChannel({"magnetometers::" + sensorName,
                                                {3, 1}, //
                                                {"mag_x", "mag_y", "mag_z"}});
        }

        // an IMU contains a gyro accelerometer and an orientation sensor
        for (const auto& sensorName : m_robotSensorBridge->getIMUsList())
        {
            ok = ok
                 && m_bufferManager.addChannel({"accelerometers::" + sensorName,
                                                {3, 1}, //
                                                {"a_x", "a_y", "a_z"}})
                 && m_bufferManager.addChannel({"gyros::" + sensorName,
                                                {3, 1}, //
                                                {"omega_x", "omega_y", "omega_z"}})
                 && m_bufferManager.addChannel({"orientations::" + sensorName,
                                                {3, 1}, //
                                                {"r", "p", "y"}});
        }
    }

    if (m_streamCartesianWrenches)
    {
        for (const auto& sensorName : m_robotSensorBridge->getCartesianWrenchesList())
        {
            ok = ok
                 && m_bufferManager.addChannel({"cartesian_wrenches::" + sensorName,
                                                {6, 1}, //
                                                {"f_x", "f_y", "f_z", "mu_x", "mu_y", "mu_z"}});
        }
    }

    if (m_streamTemperatureSensors)
    {
        for (const auto& sensorName : m_robotSensorBridge->getTemperatureSensorsList())
        {
            ok = ok
                 && m_bufferManager.addChannel({"temperatures::" + sensorName,
                                                {1, 1}, //
                                                {"temperature"}});
        }
    }

    // resize the temporary vectors
    m_jointSensorBuffer.resize(dofs);

    // open the TextLogging port
    ok = ok && m_textLoggingPort.open(m_textLoggingPortName);
    // run the thread
    m_lookForNewLogsThread = std::thread([this] { this->lookForNewLogs(); });

    // run the thread for reading the exogenous signals
    m_lookForNewExogenousSignalThread = std::thread([this] { this->lookForExogenousSignals(); });

    // The user can avoid to record the camera
    if (m_cameraBridge != nullptr)
    {
        ok = ok && m_cameraBridge->getRGBCamerasList(m_rgbCamerasList);
        for (const auto& camera : m_rgbCamerasList)
        {
            if (m_videoWriters[camera].rgb->saveMode == VideoWriter::SaveMode::Video)
            {
                if (!this->openVideoWriter(m_videoWriters[camera].rgb,
                                           camera,
                                           "rgb",
                                           m_cameraBridge->getMetaData()
                                               .bridgeOptions.rgbImgDimensions))
                {
                    log()->error("{} Unable open the video writer for the camera named {}.",
                                 logPrefix,
                                 camera);
                    return false;
                }
            } else
            {
                if (!this->createFramesFolder(m_videoWriters[camera].rgb, camera, "rgb"))
                {
                    log()->error("{} Unable to create the folder to store the frames for the "
                                 "camera named {}.",
                                 logPrefix,
                                 camera);
                    return false;
                }
            }
            ok = ok
                 && m_bufferManager.addChannel({"camera::" + camera + "::rgb",
                                                {1, 1}, //
                                                {"timestamp"}});
        }

        ok = ok && m_cameraBridge->getRGBDCamerasList(m_rgbdCamerasList);
        for (const auto& camera : m_rgbdCamerasList)
        {
            if (m_videoWriters[camera].rgb->saveMode == VideoWriter::SaveMode::Video)
            {
                if (!this->openVideoWriter(m_videoWriters[camera].rgb,
                                           camera,
                                           "rgb",
                                           m_cameraBridge->getMetaData()
                                               .bridgeOptions.rgbdImgDimensions))
                {
                    log()->error("{} Unable open the video writer for the rgbd camera named {}.",
                                 logPrefix,
                                 camera);
                    return false;
                }
            } else
            {
                if (!this->createFramesFolder(m_videoWriters[camera].rgb, camera, "rgb"))
                {
                    log()->error("{} Unable to create the folder to store the frames for the "
                                 "camera named {}.",
                                 logPrefix,
                                 camera);
                    return false;
                }
            }
            if (m_videoWriters[camera].depth->saveMode == VideoWriter::SaveMode::Video)
            {
                if (!this->openVideoWriter(m_videoWriters[camera].depth,
                                           camera,
                                           "depth",
                                           m_cameraBridge->getMetaData()
                                               .bridgeOptions.rgbdImgDimensions))
                {
                    log()->error("{} Unable open the video writer for the rgbd camera named {}.",
                                 logPrefix,
                                 camera);
                    return false;
                }
            } else
            {
                if (!this->createFramesFolder(m_videoWriters[camera].depth, camera, "depth"))
                {
                    log()->error("{} Unable to create the folder to store the frames for the "
                                 "camera named {}.",
                                 logPrefix,
                                 camera);
                    return false;
                }
            }

            ok = ok
                 && m_bufferManager.addChannel({"camera::" + camera + "::rgb",
                                                {1, 1}, //
                                                {"timestamp"}});

            ok = ok
                 && m_bufferManager.addChannel({"camera::" + camera + "::depth",
                                                {1, 1}, //
                                                {"timestamp"}});
        }

        if (ok)
        {
            // using C++17 it is not possible to use a structured binding in the for loop, i.e. for
            // (auto& [key, val] : m_videoWriters) since Lambda implicit capture fails with variable
            // declared from structured binding.
            // As explained in http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2017/p0588r1.html
            // If a lambda-expression [...] captures a structured binding (explicitly or
            // implicitly), the program is ill-formed.
            // you can find further information here:
            // https://stackoverflow.com/questions/46114214/lambda-implicit-capture-fails-with-variable-declared-from-structured-binding
            // Note if one day we will support c++20 we can use structured binding see
            // https://en.cppreference.com/w/cpp/language/structured_binding
            for (auto iter = m_videoWriters.begin(); iter != m_videoWriters.end(); ++iter)
            {
                // start a separate the thread for each camera
                iter->second.videoThread
                    = std::thread([this, iter] { this->recordVideo(iter->first, iter->second); });
            }
        }
    }

    ok = ok
         && m_bufferManager.setSaveCallback(
             [this](const std::string& filePrefix,
                    const robometry::SaveCallbackSaveMethod& method) -> bool {
                 return this->saveCallback(filePrefix, method);
             });

    if (ok)
    {
        return start();
    }

    return ok;
}

bool YarpRobotLoggerDevice::openVideoWriter(
    std::shared_ptr<VideoWriter::ImageSaver> imageSaver,
    const std::string& camera,
    const std::string& imageType,
    const std::unordered_map<std::string, std::pair<std::size_t, std::size_t>>& imgDimensions)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::openVideoWriter]";
    if (imageSaver == nullptr)
    {
        log()->error("{} It seems that the camera named {} do not support {}. This shouldn't be "
                     "possible.",
                     logPrefix,
                     camera,
                     imageType);
        return false;
    }

    const auto imgDimension = imgDimensions.find(camera);
    const auto videoWriter = m_videoWriters.find(camera);

    if (imgDimension == imgDimensions.cend() || videoWriter == m_videoWriters.cend())
    {
        log()->error("{} Unable to find the dimension of the image or the video writers for the "
                     "camera named {}.",
                     logPrefix,
                     camera);
        return false;
    }

    std::lock_guard guard(imageSaver->mutex);
    imageSaver->writer
        = std::make_shared<cv::VideoWriter>("output_" + camera + "_" + imageType + ".mp4",
                                            cv::VideoWriter::fourcc(m_videoCodecCode.at(0),
                                                                    m_videoCodecCode.at(1),
                                                                    m_videoCodecCode.at(2),
                                                                    m_videoCodecCode.at(3)),
                                            videoWriter->second.fps,
                                            cv::Size(imgDimension->second.first,
                                                     imgDimension->second.second),
                                            "rgb" == imageType);
    return true;
}

bool YarpRobotLoggerDevice::createFramesFolder(std::shared_ptr<VideoWriter::ImageSaver> imageSaver,
                                               const std::string& camera,
                                               const std::string& imageType)
{
    namespace fs = std::filesystem;

    constexpr auto logPrefix = "[YarpRobotLoggerDevice::createFramesFolder]";
    if (imageSaver == nullptr)
    {
        log()->error("{} It seems that the camera named {} do not support {}. This shouldn't be "
                     "possible.",
                     logPrefix,
                     camera,
                     imageType);
        return false;
    }

    imageSaver->framesPath = "output_" + camera + "_" + imageType;
    std::lock_guard guard(imageSaver->mutex);
    std::filesystem::create_directory(imageSaver->framesPath);
    return true;
}

void YarpRobotLoggerDevice::unpackIMU(Eigen::Ref<const analog_sensor_t> signal,
                                      Eigen::Ref<accelerometer_t> accelerometer,
                                      Eigen::Ref<gyro_t> gyro,
                                      Eigen::Ref<orientation_t> orientation)
{
    // the output consists 12 double, organized as follows:
    //  euler angles [3]
    // linear acceleration [3]
    // angular speed [3]
    // magnetic field [3]
    // http://wiki.icub.org/wiki/Inertial_Sensor
    orientation = signal.segment<3>(0);
    accelerometer = signal.segment<3>(3);
    gyro = signal.segment<3>(6);
}

void YarpRobotLoggerDevice::lookForExogenousSignals()
{
    yarp::profiler::NetworkProfiler::ports_name_set yarpPorts;

    using namespace std::chrono_literals;

    auto time = BipedalLocomotion::clock().now();
    auto oldTime = time;
    auto wakeUpTime = time;
    const std::chrono::nanoseconds lookForExogenousSignalPeriod = 1s;
    m_lookForNewExogenousSignalIsRunning = true;

    auto connectToExogeneous = [](auto& signals) -> void {
        for (auto& [name, signal] : signals)
        {
            if (!signal.connected && yarp::os::Network::exists(name))
            {
                std::lock_guard<std::mutex> lock(signal.mutex);
                signal.connected = signal.connect();
            }
        }
    };

    while (m_lookForNewExogenousSignalIsRunning)
    {
        // detect if a clock has been reset
        oldTime = time;
        time = BipedalLocomotion::clock().now();

        // if the current time is lower than old time, the timer has been reset.
        if ((time - oldTime).count() < 1e-12)
        {
            wakeUpTime = time;
        }
        wakeUpTime += lookForExogenousSignalPeriod;

        // try to connect to the exogenous signals
        connectToExogeneous(m_vectorsCollectionSignals);
        connectToExogeneous(m_vectorSignals);

        // release the CPU
        BipedalLocomotion::clock().yield();

        // sleep
        BipedalLocomotion::clock().sleepUntil(wakeUpTime);
    }
}

bool YarpRobotLoggerDevice::hasSubstring(const std::string& str,
                                         const std::vector<std::string>& substrings) const
{
    for (const auto& substring : substrings)
    {
        if (str.find(substring) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

void YarpRobotLoggerDevice::lookForNewLogs()
{
    using namespace std::chrono_literals;
    yarp::profiler::NetworkProfiler::ports_name_set yarpPorts;
    constexpr auto textLoggingPortPrefix = "/log/";

    auto time = BipedalLocomotion::clock().now();
    auto oldTime = time;
    auto wakeUpTime = time;
    const auto lookForNewLogsPeriod = 2s;
    m_lookForNewLogsIsRunning = true;

    while (m_lookForNewLogsIsRunning)
    {
        // detect if a clock has been reset
        oldTime = time;
        time = BipedalLocomotion::clock().now();
        // if the current time is lower than old time, the timer has been reset.
        if ((time - oldTime).count() < 1e-12)
        {
            wakeUpTime = time;
        }
        wakeUpTime += lookForNewLogsPeriod;

        // check for new messages
        yarp::profiler::NetworkProfiler::getPortsList(yarpPorts);
        for (const auto& port : yarpPorts)
        {
            // check if the port has not be already connected if exits, its resposive
            // it is a text logging port and it should be logged
            if ((port.name.rfind(textLoggingPortPrefix, 0) == 0)
                && (m_textLoggingPortNames.find(port.name) == m_textLoggingPortNames.end())
                && (m_textLoggingSubnames.empty()
                    || this->hasSubstring(port.name, m_textLoggingSubnames))
                && yarp::os::Network::exists(port.name))
            {
                m_textLoggingPortNames.insert(port.name);
                yarp::os::Network::connect(port.name, m_textLoggingPortName, "udp");
            }
        }

        // release the CPU
        BipedalLocomotion::clock().yield();

        // sleep
        BipedalLocomotion::clock().sleepUntil(wakeUpTime);
    }
}

void YarpRobotLoggerDevice::recordVideo(const std::string& cameraName, VideoWriter& writer)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::recordVideo]";

    auto time = BipedalLocomotion::clock().now();
    auto oldTime = time;
    auto wakeUpTime = time;
    writer.recordVideoIsRunning = true;
    const auto recordVideoPeriod = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(1.0 / double(writer.fps)));

    unsigned int imageIndex = 0;

    while (writer.recordVideoIsRunning)
    {
        // detect if a clock has been reset
        oldTime = time;
        time = BipedalLocomotion::clock().now();
        // if the current time is lower than old time, the timer has been reset.
        if ((time - oldTime).count() < 1e-12)
        {
            wakeUpTime = time;
        }
        wakeUpTime += recordVideoPeriod;

        // get the frame from the camera
        if (writer.rgb != nullptr)
        {
            if (!m_cameraBridge->getColorImage(cameraName, writer.rgb->frame))
            {
                log()->info("{} Unable to get the frame of the camera named: {}. The previous "
                            "frame "
                            "will be used.",
                            logPrefix,
                            cameraName);
            }

            // save the frame in the video writer
            if (writer.rgb->saveMode == VideoWriter::SaveMode::Video)
            {
                std::lock_guard<std::mutex> lock(writer.rgb->mutex);
                writer.rgb->writer->write(writer.rgb->frame);
            } else
            {
                assert(writer.rgb->saveMode == VideoWriter::SaveMode::Frame);

                const std::filesystem::path imgPath
                    = writer.rgb->framesPath / ("img_" + std::to_string(imageIndex) + ".png");

                cv::imwrite(imgPath.string(), writer.rgb->frame);

                // lock the the buffered manager mutex
                std::lock_guard lock(m_bufferManagerMutex);

                // TODO here we may save the frame itself
                m_bufferManager.push_back(time.count(),
                                          time.count(),
                                          "camera::" + cameraName + "::rgb");
            }
        }

        if (writer.depth != nullptr)
        {
            if (!m_cameraBridge->getDepthImage(cameraName, writer.depth->frame))
            {
                log()->info("{} Unable to get the frame of the camera named: {}. The previous "
                            "frame "
                            "will be used.",
                            logPrefix,
                            cameraName);

            } else
            {
                // If a new frame arrived the we should scale it
                writer.depth->frame = writer.depth->frame * writer.depthScale;
            }

            if (writer.depth->saveMode == VideoWriter::SaveMode::Video)
            {
                // we need to convert the image to 8bit this is required by the video writer
                cv::Mat image8Bit;
                writer.depth->frame.convertTo(image8Bit, CV_8UC1);

                // save the frame in the video writer
                std::lock_guard<std::mutex> lock(writer.depth->mutex);
                writer.depth->writer->write(image8Bit);
            } else
            {
                assert(writer.depth->saveMode == VideoWriter::SaveMode::Frame);

                const std::filesystem::path imgPath
                    = writer.depth->framesPath / ("img_" + std::to_string(imageIndex) + ".png");

                // convert the image into 16bit grayscale image
                cv::Mat image16Bit;
                writer.depth->frame.convertTo(image16Bit, CV_16UC1);
                cv::imwrite(imgPath.string(), image16Bit);

                // lock the the buffered manager mutex
                std::lock_guard lock(m_bufferManagerMutex);

                // TODO here we may save the frame itself
                m_bufferManager.push_back(time.count(),
                                          time.count(),
                                          "camera::" + cameraName + "::depth");
            }
        }

        // increase the index
        imageIndex++;

        // release the CPU
        BipedalLocomotion::clock().yield();

        if (wakeUpTime < BipedalLocomotion::clock().now())
        {
            log()->info("{} The video thread spent more time than expected to save the camera "
                        "named: {}.",
                        logPrefix,
                        cameraName);
        }

        // sleep
        BipedalLocomotion::clock().sleepUntil(wakeUpTime);
    }
}

void YarpRobotLoggerDevice::run()
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::run]";

    // get the data
    if (!m_robotSensorBridge->advance())
    {
        log()->error("{} Could not advance sensor bridge.", logPrefix);
    }

    const double time = std::chrono::duration<double>(BipedalLocomotion::clock().now()).count();

    std::lock_guard lock(m_bufferManagerMutex);
    // collect the data
    if (m_streamJointStates)
    {
        if (m_robotSensorBridge->getJointPositions(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, "joints_state::positions");
        }
        if (m_robotSensorBridge->getJointVelocities(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, "joints_state::velocities");
        }
        if (m_robotSensorBridge->getJointAccelerations(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, "joints_state::accelerations");
        }
        if (m_robotSensorBridge->getJointTorques(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, "joints_state::torques");
        }
    }

    if (m_streamMotorStates)
    {
        if (m_robotSensorBridge->getMotorPositions(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, "motors_state::positions");
        }
        if (m_robotSensorBridge->getMotorVelocities(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, "motors_state::velocities");
        }
        if (m_robotSensorBridge->getMotorAccelerations(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, "motors_state::accelerations");
        }
        if (m_robotSensorBridge->getMotorCurrents(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, "motors_state::currents");
        }
    }

    if (m_streamMotorPWM)
    {
        if (m_robotSensorBridge->getMotorPWMs(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, "motors_state::PWM");
        }
    }

    if (m_streamPIDs)
    {
        if (m_robotSensorBridge->getPidPositions(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, "PIDs");
        }
    }

    for (const auto& sensorName : m_robotSensorBridge->getSixAxisForceTorqueSensorsList())
    {
        if (m_robotSensorBridge->getSixAxisForceTorqueMeasurement(sensorName, m_ftBuffer))
        {
            m_bufferManager.push_back(m_ftBuffer, time, "FTs::" + sensorName);
        }
    }

    for (const auto& sensorname : m_robotSensorBridge->getTemperatureSensorsList())
    {
        if (m_robotSensorBridge->getTemperature(sensorname, m_ftTemperatureBuffer))
        {
            m_bufferManager.push_back({m_ftTemperatureBuffer}, time, "temperatures::" + sensorname);
        }
    }

    for (const auto& sensorName : m_robotSensorBridge->getGyroscopesList())
    {
        if (m_robotSensorBridge->getGyroscopeMeasure(sensorName, m_gyroBuffer))
        {
            m_bufferManager.push_back(m_gyroBuffer, time, "gyros::" + sensorName);
        }
    }

    for (const auto& sensorName : m_robotSensorBridge->getLinearAccelerometersList())
    {
        if (m_robotSensorBridge->getLinearAccelerometerMeasurement(sensorName,
                                                                   m_acceloremeterBuffer))
        {
            m_bufferManager.push_back(m_acceloremeterBuffer, time, "accelerometers::" + sensorName);
        }
    }

    for (const auto& sensorName : m_robotSensorBridge->getOrientationSensorsList())
    {
        if (m_robotSensorBridge->getOrientationSensorMeasurement(sensorName, m_orientationBuffer))
        {
            m_bufferManager.push_back(m_orientationBuffer, time, "orientations::" + sensorName);
        }
    }

    for (const auto& sensorName : m_robotSensorBridge->getMagnetometersList())
    {
        if (m_robotSensorBridge->getMagnetometerMeasurement(sensorName, m_magnemetometerBuffer))
        {
            m_bufferManager.push_back(m_magnemetometerBuffer, time, "magnetometers::" + sensorName);
        }
    }

    // an IMU contains a gyro accelerometer and an orientation sensor
    for (const auto& sensorName : m_robotSensorBridge->getIMUsList())
    {
        if (m_robotSensorBridge->getIMUMeasurement(sensorName, m_analogSensorBuffer))
        {
            // it will return a tuple containing the Accelerometer, the gyro and the orientatio
            this->unpackIMU(m_analogSensorBuffer,
                            m_acceloremeterBuffer,
                            m_gyroBuffer,
                            m_orientationBuffer);

            m_bufferManager.push_back(m_acceloremeterBuffer, time, "accelerometers::" + sensorName);
            m_bufferManager.push_back(m_gyroBuffer, time, "gyros::" + sensorName);
            m_bufferManager.push_back(m_orientationBuffer, time, "orientations::" + sensorName);
        }
    }

    for (const auto& sensorName : m_robotSensorBridge->getCartesianWrenchesList())
    {
        if (m_robotSensorBridge->getCartesianWrench(sensorName, m_ftBuffer))
        {
            m_bufferManager.push_back(m_ftBuffer, time, "cartesian_wrenches::" + sensorName);
        }
    }

    std::string signalFullName;
    for (auto& [name, signal] : m_vectorsCollectionSignals)
    {
        std::lock_guard<std::mutex> lock(signal.mutex);
        BipedalLocomotion::YarpUtilities::VectorsCollection* collection = signal.port.read(false);
        if (collection != nullptr)
        {
            if (!signal.dataArrived)
            {
                for (const auto& [key, vector] : collection->vectors)
                {
                    signalFullName = signal.signalName + "::" + key;
                    m_bufferManager.addChannel({signalFullName, {vector.size(), 1}});
                }
                signal.dataArrived = true;
            }

            for (const auto& [key, vector] : collection->vectors)
            {
                signalFullName = signal.signalName + "::" + key;
                m_bufferManager.push_back(vector, time, signalFullName);
            }
        }
    }

    for (auto& [name, signal] : m_vectorSignals)
    {
        std::lock_guard<std::mutex> lock(signal.mutex);
        yarp::sig::Vector* vector = signal.port.read(false);
        if (vector != nullptr)
        {
            if (!signal.dataArrived)
            {
                m_bufferManager.addChannel({signal.signalName, {vector->size(), 1}});
                signal.dataArrived = true;
            }
            m_bufferManager.push_back(*vector, time, signal.signalName);
        }
    }

    int bufferportSize = m_textLoggingPort.getPendingReads();
    BipedalLocomotion::TextLoggingEntry msg;

    while (bufferportSize > 0)
    {
        yarp::os::Bottle* b = m_textLoggingPort.read(false);
        if (b != nullptr)
        {
            msg = BipedalLocomotion::TextLoggingEntry::deserializeMessage(*b, std::to_string(time));
            if (msg.isValid)
            {
                signalFullName = msg.portSystem + "::" + msg.portPrefix + "::" + msg.processName
                                 + "::p" + msg.processPID;

                // matlab does not support the character - as a key of a struct
                findAndReplaceAll(signalFullName, "-", "_");

                // if it is the first time this signal is seen by the device the channel is added
                if (m_textLogsStoredInManager.find(signalFullName)
                    == m_textLogsStoredInManager.end())
                {
                    m_bufferManager.addChannel({signalFullName, {1, 1}});
                    m_textLogsStoredInManager.insert(signalFullName);
                }

                m_bufferManager.push_back(msg, time, signalFullName);
            }
            bufferportSize = m_textLoggingPort.getPendingReads();
        } else
        {
            break;
        }
    }
}

bool YarpRobotLoggerDevice::saveCallback(const std::string& fileName,
                                         const robometry::SaveCallbackSaveMethod& method)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::saveCallback]";

    auto codeStatus = [](const std::string& cmd, const std::string& head) -> std::string {
        std::stringstream processStream, stream;

        // run the process
        TinyProcessLib::Process process(cmd, "", [&](const char* bytes, size_t n) -> void {
            processStream << std::string(bytes, n);
        });

        // if the process status is ok we can save the output
        auto exitStatus = process.get_exit_status();
        if (exitStatus == 0)
        {
            stream << "### " << head << std::endl;
            stream << "```" << std::endl;
            stream << processStream.str() << std::endl;
            stream << "```" << std::endl;
        }
        return stream.str();
    };

    auto saveVideo = [&fileName, logPrefix](std::shared_ptr<VideoWriter::ImageSaver> imageSaver,
                                            const std::string& camera,
                                            const std::string& videoTypePostfix) -> bool {
        if (imageSaver == nullptr)
        {
            log()->error("{} The camera named {} do not expose the rgb image. This should't be "
                         "possible.",
                         logPrefix,
                         camera);
            return false;
        }

        std::string temp = fileName + "_" + camera + "_" + videoTypePostfix;
        std::string oldName = "output_" + camera + "_" + videoTypePostfix;

        // release the writer
        std::lock_guard<std::mutex> lock(imageSaver->mutex);
        if (imageSaver->saveMode == VideoWriter::SaveMode::Video)
        {
            // the name of the files contains mp4
            temp += ".mp4";
            oldName += ".mp4";

            imageSaver->writer->release();
        }

        // rename the file associated to the camera
        std::filesystem::rename(oldName, temp);

        return true;
    };

    // save the video if there is any
    for (const auto& camera : m_rgbCamerasList)
    {
        if (!saveVideo(m_videoWriters[camera].rgb, camera, "rgb"))
        {
            log()->error("{} Unable to save the rgb for the camera named {}", logPrefix, camera);
            return false;
        }

        if (method != robometry::SaveCallbackSaveMethod::periodic)
        {
            continue;
        }

        if (m_videoWriters[camera].rgb->saveMode == VideoWriter::SaveMode::Video)
        {
            if (!this->openVideoWriter(m_videoWriters[camera].rgb,
                                       camera,
                                       "rgb",
                                       m_cameraBridge->getMetaData().bridgeOptions.rgbImgDimensions))
            {
                log()->error("{} Unable to open a video writer fro the camera named {}.",
                             logPrefix,
                             camera);
                return false;
            }
        } else
        {
            if (!this->createFramesFolder(m_videoWriters[camera].rgb, camera, "rgb"))
            {
                log()->error("{} Unable to create the folder associated to the frames of the "
                             "camera named {}.",
                             logPrefix,
                             camera);
                return false;
            }
        }
    }

    for (const auto& camera : m_rgbdCamerasList)
    {
        if (!saveVideo(m_videoWriters[camera].rgb, camera, "rgb"))
        {
            log()->error("{} Unable to save the rgb for the camera named {}", logPrefix, camera);
            return false;
        }

        if (!saveVideo(m_videoWriters[camera].depth, camera, "depth"))
        {
            log()->error("{} Unable to save the depth for the camera named {}", logPrefix, camera);
            return false;
        }

        if (method != robometry::SaveCallbackSaveMethod::periodic)
        {
            continue;
        }

        if (m_videoWriters[camera].rgb->saveMode == VideoWriter::SaveMode::Video)
        {

            if (!this->openVideoWriter(m_videoWriters[camera].rgb,
                                       camera,
                                       "rgb",
                                       m_cameraBridge->getMetaData()
                                           .bridgeOptions.rgbdImgDimensions))
            {
                log()->error("{} Unable to open a video writer fro the camera named {}.",
                             logPrefix,
                             camera);
                return false;
            }
        } else
        {
            if (!this->createFramesFolder(m_videoWriters[camera].rgb, camera, "rgb"))
            {
                log()->error("{} Unable to create the folder associated to the frames of the "
                             "camera named {}.",
                             logPrefix,
                             camera);
                return false;
            }
        }
        if (m_videoWriters[camera].depth->saveMode == VideoWriter::SaveMode::Video)
        {
            if (!this->openVideoWriter(m_videoWriters[camera].depth,
                                       camera,
                                       "depth",
                                       m_cameraBridge->getMetaData()
                                           .bridgeOptions.rgbdImgDimensions))
            {
                log()->error("{} Unable to open a video writer for the depth camera named {}.",
                             logPrefix,
                             camera);
                return false;
            }
        } else
        {
            if (!this->createFramesFolder(m_videoWriters[camera].depth, camera, "depth"))
            {
                log()->error("{} Unable to create the folder associated to the depth frames of the "
                             "camera named {}.",
                             logPrefix,
                             camera);
                return false;
            }
        }
    }

    // save the status of the code
    std::ofstream file(fileName + ".md");
    file << "# " << fileName << std::endl;
    file << "File containing all the installed software required to replicate the experiment.  "
         << std::endl;

    if (m_codeStatusCmdPrefixes.empty())
    {
        file << codeStatus("bash "
                           "${ROBOTOLOGY_SUPERBUILD_SOURCE_DIR}/scripts/robotologyGitStatus.sh",
                           "ROBOTOLOGY");
        file << codeStatus("apt list --installed", "APT");
    } else
    {
        for (const auto& prefix : m_codeStatusCmdPrefixes)
        {
            file << "## `" << prefix << "`" << std::endl;
            file << codeStatus(prefix
                                   + " \"bash "
                                     "${ROBOTOLOGY_SUPERBUILD_SOURCE_DIR}/scripts/"
                                     "robotologyGitStatus.sh\"",
                               "ROBOTOLOGY");
            file << codeStatus(prefix + " \"apt list --installed\"", "APT");
        }
    }

    file.close();

    return true;
}

bool YarpRobotLoggerDevice::detachAll()
{
    if (isRunning())
    {
        stop();
    }

    return true;
}

bool YarpRobotLoggerDevice::close()
{
    // stop all the video thread
    for (auto& [cameraName, writer] : m_videoWriters)
    {
        writer.recordVideoIsRunning = false;
    }

    // close all the thread associated to the video logging
    for (auto& [cameraName, writer] : m_videoWriters)
    {
        if (writer.videoThread.joinable())
        {
            writer.videoThread.join();
            writer.videoThread = std::thread();
        }
    }

    // close the thread associated to the text logging polling
    m_lookForNewLogsIsRunning = false;
    if (m_lookForNewLogsThread.joinable())
    {
        m_lookForNewLogsThread.join();
        m_lookForNewLogsThread = std::thread();
    }

    m_lookForNewExogenousSignalIsRunning = false;
    if (m_lookForNewExogenousSignalThread.joinable())
    {
        m_lookForNewExogenousSignalThread.join();
        m_lookForNewExogenousSignalThread = std::thread();
    }

    return true;
}
