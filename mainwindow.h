#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "xrfcinelooprcv.h"

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();

    void Init(const QString& savedir, const QString &fileextension, const unsigned int port, const long eostudy_timeout = -1);
    void Start();
    void Stop();
    void Wait(unsigned long time_in_milliseconds);

private:
    Ui::MainWindow *ui;
    QString mSaveDir;
    CineLoopRcv* mLoopRcv{nullptr};
};

#endif // MAINWINDOW_H
