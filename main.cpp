#include <QApplication>
#include <iostream>
#include <opencv2/opencv.hpp>
#include "mainwindow.h"

int main(int argc, char* argv[]) {
    std::cout << cv::getBuildInformation() << std::endl;

    QApplication app(argc, argv);
    app.setApplicationName("RTSP 멀티채널 뷰어");

    MainWindow w;
    w.show();

    return app.exec();
}
