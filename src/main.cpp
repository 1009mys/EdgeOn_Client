#include <QApplication>
#include <iostream>
#include <opencv2/opencv.hpp>
#include "mainwindow.h"
#include "DBManager.h"



int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName("EdgeOn");
    QCoreApplication::setOrganizationDomain("edgeon.local");
    QCoreApplication::setApplicationName("RTSP 멀티채널 뷰어");

    DBManager& dbManager = DBManager::instance("db.db");
    dbManager.initialize();

    std::cout << cv::getBuildInformation() << std::endl;


    MainWindow w;
    w.show();

    return app.exec();
}
