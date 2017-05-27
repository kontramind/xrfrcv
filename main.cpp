#include "mainwindow.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    MainWindow w;
    w.Init("C:/dev/data/received/", ".dcm", 104);
    w.Start();
    w.show();
    return a.exec();
}
