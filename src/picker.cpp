#include "picker.h"

#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include <QApplication>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QFormLayout>

#include <vector>
#include <string>

static int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

struct DeviceInfo {
    std::string path;
    std::string name;
    struct Format {
        uint32_t pixfmt;
        std::string description;
        struct Size {
            int width, height;
        };
        std::vector<Size> sizes;
    };
    std::vector<Format> formats;
};

static const char *pixfmt_name(uint32_t fmt)
{
    switch (fmt) {
    case V4L2_PIX_FMT_NV12:  return "NV12 (4:2:0)";
    case V4L2_PIX_FMT_YUYV:  return "YUYV (4:2:2)";
    case V4L2_PIX_FMT_UYVY:  return "UYVY (4:2:2)";
    case V4L2_PIX_FMT_MJPEG: return "MJPEG";
    default:                  return "Unknown";
    }
}

static std::vector<DeviceInfo> enumerate_devices()
{
    std::vector<DeviceInfo> devices;

    /* Only consider formats we actually support rendering */
    uint32_t supported[] = {
        V4L2_PIX_FMT_NV12,
        V4L2_PIX_FMT_YUYV,
        V4L2_PIX_FMT_UYVY,
    };
    int n_supported = sizeof(supported) / sizeof(supported[0]);

    DIR *dir = opendir("/dev");
    if (!dir)
        return devices;

    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "video", 5) != 0)
            continue;

        char path[64];
        snprintf(path, sizeof(path), "/dev/%s", ent->d_name);

        int fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0)
            continue;

        struct v4l2_capability cap;
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 ||
            !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
            !(cap.capabilities & V4L2_CAP_STREAMING)) {
            close(fd);
            continue;
        }

        DeviceInfo dev;
        dev.path = path;
        dev.name = reinterpret_cast<const char *>(cap.card);

        /* Enumerate formats */
        struct v4l2_fmtdesc fmtdesc = {};
        fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        while (xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
            /* Only include formats we can render */
            bool is_supported = false;
            for (int i = 0; i < n_supported; i++) {
                if (fmtdesc.pixelformat == supported[i]) {
                    is_supported = true;
                    break;
                }
            }

            if (is_supported) {
                DeviceInfo::Format fmt;
                fmt.pixfmt = fmtdesc.pixelformat;
                fmt.description = pixfmt_name(fmtdesc.pixelformat);

                /* Enumerate frame sizes for this format */
                struct v4l2_frmsizeenum frmsize = {};
                frmsize.pixel_format = fmtdesc.pixelformat;
                while (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
                    if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                        fmt.sizes.push_back({
                            static_cast<int>(frmsize.discrete.width),
                            static_cast<int>(frmsize.discrete.height)
                        });
                    }
                    frmsize.index++;
                }

                dev.formats.push_back(fmt);
            }
            fmtdesc.index++;
        }

        if (!dev.formats.empty())
            devices.push_back(dev);

        close(fd);
    }

    closedir(dir);
    return devices;
}

extern "C" int picker_show(struct picker_result *result, bool auto_select)
{
    auto devices = enumerate_devices();

    if (devices.empty()) {
        LOG_ERROR("No capture devices found");
        return -1;
    }

    if (auto_select && devices.size() == 1) {
        auto &dev = devices[0];
        snprintf(result->device_path, sizeof(result->device_path),
                 "%s", dev.path.c_str());
        result->pixfmt = 0;
        result->width = 0;
        result->height = 0;
        result->scale_mode = -1;
        result->color_range = -1;
        LOG_INFO("Auto-selected: %s (%s)", dev.name.c_str(), dev.path.c_str());
        return 0;
    }

    /* Qt needs argc/argv */
    int argc = 0;
    QApplication app(argc, nullptr);

    QDialog dialog;
    dialog.setWindowTitle("llccpv — Select Device");
    dialog.setMinimumWidth(450);

    auto *layout = new QVBoxLayout(&dialog);

    auto *form = new QFormLayout;
    layout->addLayout(form);

    /* Device selector */
    auto *deviceCombo = new QComboBox;
    for (auto &dev : devices)
        deviceCombo->addItem(
            QString::fromStdString(dev.name + "  (" + dev.path + ")"));
    form->addRow("Device:", deviceCombo);

    /* Format selector */
    auto *formatCombo = new QComboBox;
    form->addRow("Format:", formatCombo);

    /* Resolution selector */
    auto *resCombo = new QComboBox;
    resCombo->addItem("Auto (detect from source)", QVariant(0));
    form->addRow("Resolution:", resCombo);

    /* Scaling algorithm selector */
    auto *scaleCombo = new QComboBox;
    scaleCombo->addItem("Nearest Neighbor", QVariant(0));
    scaleCombo->addItem("Bilinear", QVariant(1));
    scaleCombo->addItem("Sharp Bilinear", QVariant(2));
    scaleCombo->setCurrentIndex(1);
    form->addRow("Scaling:", scaleCombo);

    /* Color range selector */
    auto *rangeCombo = new QComboBox;
    rangeCombo->addItem("Limited (TV, 16-235)", QVariant(0));
    rangeCombo->addItem("Full (PC, 0-255)", QVariant(1));
    rangeCombo->setCurrentIndex(0); /* most HDMI sources use limited */
    form->addRow("Black Level:", rangeCombo);

    /* Populate formats based on selected device */
    auto updateFormats = [&]() {
        formatCombo->clear();
        int idx = deviceCombo->currentIndex();
        if (idx < 0 || idx >= static_cast<int>(devices.size()))
            return;
        auto &dev = devices[idx];
        formatCombo->addItem("Auto (best available)", QVariant(0u));
        for (auto &fmt : dev.formats)
            formatCombo->addItem(
                QString::fromStdString(fmt.description),
                QVariant(fmt.pixfmt));
    };

    /* Populate resolutions based on selected format */
    auto updateResolutions = [&]() {
        resCombo->clear();
        resCombo->addItem("Auto (detect from source)", QVariant(0));

        int devIdx = deviceCombo->currentIndex();
        int fmtIdx = formatCombo->currentIndex() - 1; /* -1 for "Auto" entry */
        if (devIdx < 0 || devIdx >= static_cast<int>(devices.size()))
            return;
        if (fmtIdx < 0 || fmtIdx >= static_cast<int>(devices[devIdx].formats.size()))
            return;

        auto &fmt = devices[devIdx].formats[fmtIdx];
        for (auto &sz : fmt.sizes) {
            QString label = QString("%1x%2").arg(sz.width).arg(sz.height);
            resCombo->addItem(label, QVariant((sz.width << 16) | sz.height));
        }
    };

    QObject::connect(deviceCombo, &QComboBox::currentIndexChanged,
                     [&](int) { updateFormats(); });
    QObject::connect(formatCombo, &QComboBox::currentIndexChanged,
                     [&](int) { updateResolutions(); });

    updateFormats();

    /* Buttons */
    auto *btnLayout = new QHBoxLayout;
    layout->addLayout(btnLayout);

    auto *refreshBtn = new QPushButton("Refresh");
    auto *startBtn = new QPushButton("Start");
    startBtn->setDefault(true);
    btnLayout->addWidget(refreshBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(startBtn);

    bool accepted = false;

    QObject::connect(refreshBtn, &QPushButton::clicked, [&]() {
        devices = enumerate_devices();
        deviceCombo->clear();
        for (auto &dev : devices)
            deviceCombo->addItem(
                QString::fromStdString(dev.name + "  (" + dev.path + ")"));
        updateFormats();
    });

    QObject::connect(startBtn, &QPushButton::clicked, [&]() {
        accepted = true;
        dialog.close();
    });

    dialog.exec();

    if (!accepted)
        return -1;

    int devIdx = deviceCombo->currentIndex();
    if (devIdx < 0 || devIdx >= static_cast<int>(devices.size()))
        return -1;

    snprintf(result->device_path, sizeof(result->device_path),
             "%s", devices[devIdx].path.c_str());

    /* Format */
    QVariant fmtVar = formatCombo->currentData();
    result->pixfmt = fmtVar.isValid() ? fmtVar.toUInt() : 0;

    /* Resolution */
    QVariant resVar = resCombo->currentData();
    if (resVar.isValid() && resVar.toInt() != 0) {
        int packed = resVar.toInt();
        result->width = (packed >> 16) & 0xFFFF;
        result->height = packed & 0xFFFF;
    } else {
        result->width = 0;
        result->height = 0;
    }

    /* Scaling */
    QVariant scaleVar = scaleCombo->currentData();
    result->scale_mode = scaleVar.isValid() ? scaleVar.toInt() : -1;

    /* Color range */
    QVariant rangeVar = rangeCombo->currentData();
    result->color_range = rangeVar.isValid() ? rangeVar.toInt() : -1;

    LOG_INFO("Selected: %s, fmt=0x%08x, res=%dx%d, scale=%d",
             result->device_path, result->pixfmt,
             result->width, result->height, result->scale_mode);
    return 0;
}
