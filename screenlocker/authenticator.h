/********************************************************************
 KSld - the KDE Screenlocker Daemon
 This file is part of the KDE project.

Copyright (C) 2014 Martin Gräßlin <mgraesslin@kde.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/
#ifndef AUTHENTICATOR_H
#define AUTHENTICATOR_H

#include <QElapsedTimer>
#include <QObject>

class QSocketNotifier;
class QTimer;
class KCheckPass;

enum class AuthenticationMode {
    Delayed,
    Direct,
};

class Authenticator : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool graceLocked READ isGraceLocked NOTIFY graceLockedChanged)
    // Fingerprint login is enabled and a fingerprint conversation is running
    Q_PROPERTY(bool fingerprintAvailable READ isFingerprintAvailable NOTIFY fingerprintAvailableChanged)
    // Face login (webcam): "" when off or given up, "looking" while the camera
    // looks, then "failed", "liveness" (the face didn't move) or "unavailable"
    // (no camera, or another program uses it)
    Q_PROPERTY(QString faceState READ faceState NOTIFY faceStateChanged)
public:
    explicit Authenticator(AuthenticationMode mode = AuthenticationMode::Direct, QObject *parent = nullptr);
    ~Authenticator() override;

    bool isGraceLocked() const;
    bool isFingerprintAvailable() const;
    QString faceState() const { return m_faceState; }

public Q_SLOTS:
    void tryUnlock(const QString &password);

    // Runs the "lingmo-screenlocker-fingerprint" PAM service next to the
    // password one when /etc/lingmo-fingerprint-enabled exists, so a finger
    // unlocks without typing anything. Restarted after each attempt.
    void startFingerprint();
    // Runs the "lingmo-screenlocker-face" PAM service (lingmo-faceauth through
    // pam_exec) next to the password one when /etc/lingmo-face-enabled exists
    // and the user enrolled a face. One look of a few seconds; another one only
    // after key or mouse input (userActivity), so the camera isn't kept on.
    void startFace();
    // Key or mouse input: restarts the fingerprint conversation if it went idle,
    // and looks for the face again if the last look ended a while ago.
    void userActivity();

Q_SIGNALS:
    void failed();
    void succeeded();
    void graceLockedChanged();
    void message(const QString &msg); // don't remove the "msg" param, used in QML!!!
    void error(const QString &err); // don't remove the "err" param, used in QML!!!
    void fingerprintAvailableChanged();
    void fingerprintError(const QString &err); // e.g. "Failed to match fingerprint", used in QML
    void faceStateChanged();

private:
    void setupCheckPass();
    void onFingerprintFailed();
    void onFaceFailed();
    void onFaceMessage(const QString &msg);
    void setFaceState(const QString &state);
    QTimer *m_graceLockTimer;
    KCheckPass *m_checkPass;

    KCheckPass *m_fingerprint = nullptr;
    QTimer *m_fingerprintRestartTimer;
    QElapsedTimer m_fingerprintElapsed;
    int m_fingerprintQuickFailures = 0;
    int m_fingerprintRounds = 0;
    bool m_fingerprintDisabled = false;

    KCheckPass *m_face = nullptr;
    QString m_faceState;
    QString m_faceResult;
    QElapsedTimer m_faceElapsed;
    QElapsedTimer m_faceEnded;
    int m_faceRounds = 0;
    bool m_faceDisabled = false;
};

class KCheckPass : public QObject
{
    Q_OBJECT
public:
    // method selects the PAM service: "classic" is lingmo-screenlocker,
    // anything else lingmo-screenlocker-<method>
    explicit KCheckPass(AuthenticationMode mode, QObject *parent = nullptr,
                        const QString &method = QStringLiteral("classic"));
    ~KCheckPass() override;

    void start();

    bool isReady() const
    {
        return m_ready;
    }

    void setPassword(const QString &password)
    {
        m_password = password;
    }

    void startAuth();

Q_SIGNALS:
    void failed();
    void succeeded();
    void message(const QString &);
    void error(const QString &);

private Q_SLOTS:
    void handleVerify();

private:
    void cantCheck();
    void reapVerify();
    // kcheckpass interface
    int Reader(void *buf, int count);
    bool GRead(void *buf, int count);
    bool GWrite(const void *buf, int count);
    bool GSendInt(int val);
    bool GSendStr(const char *buf);
    bool GSendArr(int len, const char *buf);
    bool GRecvInt(int *val);
    bool GRecvArr(char **buf);

    QString m_password;
    QSocketNotifier *m_notifier;
    int m_pid;
    int m_fd;
    bool m_ready = false;
    AuthenticationMode m_mode;
    QString m_method;
};

#endif
