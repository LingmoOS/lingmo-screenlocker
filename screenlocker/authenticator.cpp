/********************************************************************
 KSld - the KDE Screenlocker Daemon
 This file is part of the KDE project.

Copyright (C) 1999 Martin R. Jones <mjones@kde.org>
Copyright (C) 2002 Luboš Luňák <l.lunak@kde.org>
Copyright (C) 2003 Oswald Buddenhagen <ossi@kde.org>
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
#include "authenticator.h"

#include "kcheckpass-enums.h"

// Qt
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSocketNotifier>
#include <QTimer>

// system
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
// Written by lingmo-settings' fingerprint-pam helper when fingerprint login is on.
QString fingerprintMarker()
{
    const QString path = qEnvironmentVariable("LINGMO_FINGERPRINT_MARKER");
    return path.isEmpty() ? QStringLiteral("/etc/lingmo-fingerprint-enabled") : path;
}

// A fingerprint conversation that ends this fast didn't wait for a finger: no
// reader, no enrolled finger or a broken PAM service. After a few of those in a
// row fingerprint unlocking is given up until the next lock.
constexpr qint64 QuickFailureMs = 3000;
constexpr int MaxQuickFailures = 3;
// pam_fprintd gives up after ~30 s or three wrong fingers. Restart it on our own
// this many times, then wait for key or mouse input (saves the reader's power).
constexpr int MaxIdleRounds = 10;

// Written by lingmo-settings' face-pam helper when face login is on.
QString faceMarker()
{
    const QString path = qEnvironmentVariable("LINGMO_FACE_MARKER");
    return path.isEmpty() ? QStringLiteral("/etc/lingmo-face-enabled") : path;
}

// The user's enrolled face (lingmo-faceauth enroll, from Settings)
QString faceStore()
{
    const QString path = qEnvironmentVariable("LINGMO_FACE_STORE");
    if (!path.isEmpty())
        return path;
    const passwd *pw = ::getpwuid(::getuid());
    if (!pw || !pw->pw_dir)
        return QString();
    return QFile::decodeName(pw->pw_dir) + QStringLiteral("/.local/share/lingmoos/face/")
        + QString::fromLocal8Bit(pw->pw_name) + QStringLiteral(".dat");
}

// A face look that ended this fast without a word from lingmo-faceauth never
// ran (PAM service or lingmo-faceauth missing, account locked by faillock): the
// face is given up until the next lock.
constexpr qint64 FaceQuickFailureMs = 1500;
// Look again on key or mouse input only this long after the last look ended...
constexpr qint64 FaceRetryMs = 5000;
// ...and not more often than this per lock.
constexpr int MaxFaceRounds = 10;
}

Authenticator::Authenticator(AuthenticationMode mode, QObject *parent)
    : QObject(parent)
    , m_graceLockTimer(new QTimer(this))
    , m_checkPass(nullptr)
    , m_fingerprintRestartTimer(new QTimer(this))
{
    m_graceLockTimer->setSingleShot(true);
    m_graceLockTimer->setInterval(1500);
    connect(m_graceLockTimer, &QTimer::timeout, this, &Authenticator::graceLockedChanged);

    m_fingerprintRestartTimer->setSingleShot(true);
    connect(m_fingerprintRestartTimer, &QTimer::timeout, this, &Authenticator::startFingerprint);

    if (mode == AuthenticationMode::Delayed) {
        m_checkPass = new KCheckPass(AuthenticationMode::Delayed, this);
        setupCheckPass();
    }
}

Authenticator::~Authenticator() = default;

void Authenticator::tryUnlock(const QString &password)
{
    if (isGraceLocked()) {
        Q_EMIT failed();
        return;
    }
    m_graceLockTimer->start();
    Q_EMIT graceLockedChanged();

    if (!m_checkPass) {
        m_checkPass = new KCheckPass(AuthenticationMode::Direct, this);
        m_checkPass->setPassword(password);
        setupCheckPass();
    } else {
        if (!m_checkPass->isReady()) {
            Q_EMIT failed();
            return;
        }
        m_checkPass->setPassword(password);
        m_checkPass->startAuth();
    }
}

void Authenticator::setupCheckPass()
{
    connect(m_checkPass, &KCheckPass::succeeded, this, &Authenticator::succeeded);
    connect(m_checkPass, &KCheckPass::failed, this, &Authenticator::failed);
    connect(m_checkPass, &KCheckPass::message, this, &Authenticator::message);
    connect(m_checkPass, &KCheckPass::error, this, &Authenticator::error);
    connect(m_checkPass, &KCheckPass::destroyed, this, [this] {
        m_checkPass = nullptr;
    });
    m_checkPass->start();
}

bool Authenticator::isGraceLocked() const
{
    return m_graceLockTimer->isActive();
}

bool Authenticator::isFingerprintAvailable() const
{
    return !m_fingerprintDisabled && (m_fingerprint || m_fingerprintRestartTimer->isActive());
}

void Authenticator::startFingerprint()
{
    if (m_fingerprint || m_fingerprintDisabled || !QFile::exists(fingerprintMarker())) {
        return;
    }

    const bool wasAvailable = isFingerprintAvailable();
    m_fingerprintRestartTimer->stop();

    // Only succeeded matters here: a failed round is restarted, and the
    // password field keeps its own conversation.
    m_fingerprint = new KCheckPass(AuthenticationMode::Direct, this, QStringLiteral("fingerprint"));
    connect(m_fingerprint, &KCheckPass::succeeded, this, &Authenticator::succeeded);
    connect(m_fingerprint, &KCheckPass::failed, this, &Authenticator::onFingerprintFailed);
    connect(m_fingerprint, &KCheckPass::error, this, &Authenticator::fingerprintError);
    connect(m_fingerprint, &KCheckPass::destroyed, this, [this](QObject *object) {
        if (m_fingerprint == object) {
            m_fingerprint = nullptr;
        }
    });

    m_fingerprintElapsed.start();
    m_fingerprint->start();

    if (!wasAvailable) {
        Q_EMIT fingerprintAvailableChanged();
    }
}

void Authenticator::onFingerprintFailed()
{
    // The KCheckPass deletes itself (direct mode).
    if (m_fingerprint) {
        disconnect(m_fingerprint, nullptr, this, nullptr);
        m_fingerprint = nullptr;
    }

    const bool quick = m_fingerprintElapsed.elapsed() < QuickFailureMs;
    if (quick) {
        if (++m_fingerprintQuickFailures >= MaxQuickFailures) {
            m_fingerprintDisabled = true;
            Q_EMIT fingerprintAvailableChanged();
            return;
        }
    } else {
        m_fingerprintQuickFailures = 0;
    }

    if (++m_fingerprintRounds > MaxIdleRounds) {
        // Wait for userActivity()
        Q_EMIT fingerprintAvailableChanged();
        return;
    }

    m_fingerprintRestartTimer->start(quick ? 2000 : 500);
}

void Authenticator::userActivity()
{
    m_fingerprintRounds = 0;
    if (!m_fingerprint && !m_fingerprintRestartTimer->isActive()) {
        startFingerprint();
    }
    if (!m_face && !m_faceDisabled && !m_faceState.isEmpty() && m_faceEnded.isValid()
            && m_faceEnded.elapsed() >= FaceRetryMs) {
        startFace();
    }
}

void Authenticator::setFaceState(const QString &state)
{
    if (m_faceState != state) {
        m_faceState = state;
        Q_EMIT faceStateChanged();
    }
}

void Authenticator::startFace()
{
    if (m_face || m_faceDisabled || m_faceRounds >= MaxFaceRounds || !QFile::exists(faceMarker())) {
        return;
    }
    const QString store = faceStore();
    if (store.isEmpty() || !QFile::exists(store)) {
        return;
    }

    ++m_faceRounds;
    m_faceResult.clear();
    m_face = new KCheckPass(AuthenticationMode::Direct, this, QStringLiteral("face"));
    connect(m_face, &KCheckPass::succeeded, this, &Authenticator::succeeded);
    connect(m_face, &KCheckPass::failed, this, &Authenticator::onFaceFailed);
    // lingmo-faceauth's "face:<what>" lines (verify --machine), through pam_exec
    connect(m_face, &KCheckPass::message, this, &Authenticator::onFaceMessage);
    connect(m_face, &KCheckPass::error, this, &Authenticator::onFaceMessage);
    connect(m_face, &KCheckPass::destroyed, this, [this](QObject *object) {
        if (m_face == object) {
            m_face = nullptr;
        }
    });

    m_faceElapsed.start();
    m_face->start();
    setFaceState(QStringLiteral("looking"));
}

void Authenticator::onFaceMessage(const QString &msg)
{
    const QString line = msg.trimmed();
    if (!line.startsWith(QLatin1String("face:"))) {
        return;
    }
    const QString what = line.mid(5);
    if (what == QLatin1String("looking")) {
        setFaceState(QStringLiteral("looking"));
    } else if (what == QLatin1String("nomatch")) {
        m_faceResult = QStringLiteral("failed");
    } else if (what == QLatin1String("liveness")) {
        m_faceResult = QStringLiteral("liveness");
    } else if (what == QLatin1String("busy") || what == QLatin1String("nocamera")) {
        m_faceResult = QStringLiteral("unavailable");
    } else if (what == QLatin1String("noenroll") || what == QLatin1String("remote") || what == QLatin1String("error")) {
        // Nothing to retry until the next lock
        m_faceResult = QStringLiteral("off");
    }
}

void Authenticator::onFaceFailed()
{
    // The KCheckPass deletes itself (direct mode).
    if (m_face) {
        disconnect(m_face, nullptr, this, nullptr);
        m_face = nullptr;
    }
    m_faceEnded.start();

    if (m_faceResult == QLatin1String("off")) {
        m_faceDisabled = true;
        setFaceState(QString());
        return;
    }
    if (m_faceResult.isEmpty() && m_faceElapsed.elapsed() < FaceQuickFailureMs) {
        m_faceDisabled = true;
        setFaceState(QString());
        return;
    }
    setFaceState(m_faceResult.isEmpty() ? QStringLiteral("failed") : m_faceResult);
}

KCheckPass::KCheckPass(AuthenticationMode mode, QObject *parent, const QString &method)
    : QObject(parent)
    , m_notifier(nullptr)
    , m_pid(0)
    , m_fd(0)
    , m_mode(mode)
    , m_method(method)
{
    if (mode == AuthenticationMode::Direct) {
        connect(this, &KCheckPass::succeeded, this, &QObject::deleteLater);
        connect(this, &KCheckPass::failed, this, &QObject::deleteLater);
    }
}

KCheckPass::~KCheckPass()
{
    reapVerify();
}

void KCheckPass::start()
{
    int sfd[2];
    char fdbuf[16];

    if (m_notifier) {
        return;
    }
    if (::socketpair(AF_LOCAL, SOCK_STREAM, 0, sfd)) {
        cantCheck();
        return;
    }
    // no allocations between fork() and exec()
    const QByteArray method = m_method.toLatin1();
    if ((m_pid = ::fork()) < 0) {
        ::close(sfd[0]);
        ::close(sfd[1]);
        cantCheck();
        return;
    }
    if (!m_pid) {
        ::close(sfd[0]);
        sprintf(fdbuf, "%d", sfd[1]);
        execlp(QFile::encodeName(QStringLiteral("ccheckpass")).data(), "kcheckpass", "-m", method.constData(), "-S", fdbuf, (char *)nullptr);
        _exit(20);
    }
    ::close(sfd[1]);
    m_fd = sfd[0];
    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &KCheckPass::handleVerify);
}

////// kckeckpass interface code

int KCheckPass::Reader(void *buf, int count)
{
    int ret, rlen;

    for (rlen = 0; rlen < count;) {
    dord:
        ret = ::read(m_fd, (void *)((char *)buf + rlen), count - rlen);
        if (ret < 0) {
            if (errno == EINTR) {
                goto dord;
            }
            if (errno == EAGAIN) {
                break;
            }
            return -1;
        }
        if (!ret) {
            break;
        }
        rlen += ret;
    }
    return rlen;
}

bool KCheckPass::GRead(void *buf, int count)
{
    return Reader(buf, count) == count;
}

bool KCheckPass::GWrite(const void *buf, int count)
{
    return ::write(m_fd, buf, count) == count;
}

bool KCheckPass::GSendInt(int val)
{
    return GWrite(&val, sizeof(val));
}

bool KCheckPass::GSendStr(const char *buf)
{
    int len = buf ? ::strlen(buf) + 1 : 0;
    return GWrite(&len, sizeof(len)) && GWrite(buf, len);
}

bool KCheckPass::GSendArr(int len, const char *buf)
{
    return GWrite(&len, sizeof(len)) && GWrite(buf, len);
}

bool KCheckPass::GRecvInt(int *val)
{
    return GRead(val, sizeof(*val));
}

bool KCheckPass::GRecvArr(char **ret)
{
    int len;
    char *buf;

    if (!GRecvInt(&len)) {
        return false;
    }
    if (!len) {
        *ret = nullptr;
        return true;
    }
    if (!(buf = (char *)::malloc(len))) {
        return false;
    }
    *ret = buf;
    if (GRead(buf, len)) {
        return true;
    } else {
        ::free(buf);
        *ret = nullptr;
        return false;
    }
}

void KCheckPass::handleVerify()
{
    m_ready = false;
    int ret;
    char *arr;

    if (GRecvInt(&ret)) {
        switch (ret) {
        case ConvGetBinary:
            if (!GRecvArr(&arr)) {
                break;
            }
            // FIXME: not supported
            cantCheck();
            if (arr) {
                ::free(arr);
            }
            return;
        case ConvGetNormal:
        case ConvGetHidden: {
            if (!GRecvArr(&arr)) {
                break;
            }

            if (m_password.isNull()) {
                GSendStr(nullptr);
            } else {
                QByteArray utf8pass = m_password.toUtf8();
                GSendStr(utf8pass.constData());
                GSendInt(IsPassword);
            }

            m_password.clear();

            if (arr) {
                ::free(arr);
            }
            return;
        }
        case ConvPutInfo:
            if (!GRecvArr(&arr)) {
                break;
            }
            Q_EMIT message(QString::fromLocal8Bit(arr));
            ::free(arr);
            return;
        case ConvPutError:
            if (!GRecvArr(&arr)) {
                break;
            }
            Q_EMIT error(QString::fromLocal8Bit(arr));
            ::free(arr);
            return;
        case ConvPutAuthSucceeded:
            Q_EMIT succeeded();
            return;
        case ConvPutAuthFailed:
            Q_EMIT failed();
            return;
        case ConvPutAuthError:
        case ConvPutAuthAbort:
            cantCheck();
            return;
        case ConvPutReadyForAuthentication:
            m_ready = true;
            if (m_mode == AuthenticationMode::Direct) {
                ::kill(m_pid, SIGUSR1);
            }
            return;
        }
    }
    if (m_mode == AuthenticationMode::Direct) {
        // ccheckpass went away: report it, so a new one is started next time
        reapVerify();
        Q_EMIT failed();
    } else {
        // we broke, let's restart the greeter
        // error code 1 will result in a restart through the system
        qApp->exit(1);
    }
}

void KCheckPass::reapVerify()
{
    if (!m_notifier) {
        // never started, or already reaped
        return;
    }
    m_notifier->setEnabled(false);
    m_notifier->deleteLater();
    m_notifier = nullptr;
    ::close(m_fd);
    int status;
    // SIGUSR2 only ends ccheckpass while it waits between two attempts; the
    // fingerprint one is usually inside pam_authenticate waiting for a finger.
    ::kill(m_pid, m_method == QLatin1String("classic") ? SIGUSR2 : SIGTERM);
    while (::waitpid(m_pid, &status, 0) < 0) {
        if (errno != EINTR) { // This should not happen ...
            cantCheck();
            return;
        }
    }
}

void KCheckPass::cantCheck()
{
    // TODO: better signal?
    Q_EMIT failed();
}

void KCheckPass::startAuth()
{
    ::kill(m_pid, SIGUSR1);
}
