#include "Job.hpp"
#include "JobModel.hpp"
#include "Project.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <cstdlib> // std::getenv
#include <cassert>

namespace meshroom
{

namespace // empty namespace
{

Attribute* getAttribute(const Job& job, const QString& stepName, const QString& attrKey,
                        QModelIndex& outIndex, Step** outStep)
{
    if(!job.steps())
        return nullptr;
    for(size_t i = 0; i < job.steps()->rowCount(); i++)
    {
        QModelIndex id = job.steps()->index(i, 0);
        Step* step = job.steps()->data(id, StepModel::ModelDataRole).value<Step*>();
        if(step && step->name() != stepName)
            continue;
        for(size_t i = 0; i < step->attributes()->rowCount(); i++)
        {
            QModelIndex id = step->attributes()->index(i, 0);
            Attribute* att =
                step->attributes()->data(id, AttributeModel::ModelDataRole).value<Attribute*>();
            if(att && att->key() != attrKey)
                continue;
            outIndex = id;
            *outStep = step;
            return att;
        }
    }
    return nullptr;
}

bool isRegisteredImage(const Job& job, const QUrl& url)
{
    ResourceModel* images = job.images();
    if(!images || images->rowCount() <= 0)
        return false;
    for(size_t i = 0; i < images->rowCount(); ++i)
    {
        QModelIndex id = images->index(i, 0);
        if(url == images->data(id, ResourceModel::UrlRole))
            return true;
    }
    return false;
}

} // empty namespace

Job::Job(Project* project)
    : _project(project)
    , _user(std::getenv("USER"))
    , _date(QDateTime::currentDateTime())
    , _name(_date.toString("yyyyMMdd_HHmmss"))
    , _steps(new StepModel(this))
    , _images(new ResourceModel(this))
{
    // compute job url
    _url = QUrl::fromLocalFile(project->url().toLocalFile()+"/reconstructions/"+_date.toString("yyyyMMdd_HHmmss"));
    // create the default graph
    createDefaultGraph();
    // activate auto-save
    autoSaveOn();
    // activate auto thumbnail selection
    QObject::connect(_images, SIGNAL(countChanged(int)), this, SLOT(selectThumbnail()));
}

void Job::setUrl(const QUrl& url)
{
    if(_url == url)
        return;
    _url = url;
    emit dataChanged();
}

void Job::setName(const QString& name)
{
    if(_name == name)
        return;
    _name = name;
    emit dataChanged();
}

void Job::setDate(const QDateTime& date)
{
    if(_date == date)
        return;
    _date = date;
    emit dataChanged();
}

void Job::setUser(const QString& user)
{
    if(_user == user)
        return;
    _user = user;
    emit dataChanged();
}

void Job::setCompletion(const float& completion)
{
    _completion = completion;
}

void Job::setStatus(const int& status)
{
    _status = status;
}

void Job::setThumbnail(const QUrl& thumbnail)
{
    _thumbnail = thumbnail;
}

void Job::setModelIndex(const QModelIndex& id)
{
    _modelIndex = id;
}

bool Job::load(const QUrl& url)
{
    // return in case the job url isn't valid
    QDir dir(url.toLocalFile());
    if(!dir.exists())
    {
        qCritical() << _name << ": malformed or empty URL '" << url.toLocalFile() << "'";
        return false;
    }
    _url = url;
    // open a file handler
    QFile jsonFile(dir.filePath("job.json"));
    if(!jsonFile.open(QIODevice::ReadOnly))
    {
        qWarning() << _name << ": unable to read the job descriptor file" << jsonFile.fileName();
        return false;
    }
    // read it and close the file handler
    QByteArray data = jsonFile.readAll();
    jsonFile.close();
    // parse data as JSON
    QJsonParseError parseError;
    QJsonDocument jsonDocument(QJsonDocument::fromJson(data, &parseError));
    if(parseError.error != QJsonParseError::NoError)
    {
        qWarning() << _name << ": malformed JSON file" << jsonFile.fileName();
        return false;
    }
    // read job attributes
    QJsonObject json = jsonDocument.object();
    deserializeFromJSON(json);
    return true;
}

bool Job::load(const Job& job)
{
    delete _images;
    delete _steps;
    _images = new ResourceModel(*(job.images()));
    _steps = new StepModel(*(job.steps()));
    _thumbnail = job.thumbnail();
    return true;
}

void Job::autoSaveOn()
{
    QObject::connect(this, SIGNAL(dataChanged()), this, SLOT(save()));
    QObject::connect(_images, SIGNAL(countChanged(int)), this, SLOT(save()));
    for(size_t i = 0; i < _steps->rowCount(); i++)
    {
        QModelIndex id = _steps->index(i, 0);
        Step* step = _steps->data(id, StepModel::ModelDataRole).value<Step*>();
        QObject::connect(step->attributes(),
                         SIGNAL(dataChanged(const QModelIndex&, const QModelIndex&)), this,
                         SLOT(save()));
    }
}

void Job::autoSaveOff()
{
    QObject::disconnect(this, SIGNAL(dataChanged()), this, SLOT(save()));
    QObject::disconnect(_images, SIGNAL(countChanged(int)), this, SLOT(save()));
    for(size_t i = 0; i < _steps->rowCount(); i++)
    {
        QModelIndex id = _steps->index(i, 0);
        Step* step = _steps->data(id, StepModel::ModelDataRole).value<Step*>();
        QObject::disconnect(step->attributes(),
                            SIGNAL(dataChanged(const QModelIndex&, const QModelIndex&)), this,
                            SLOT(save()));
    }
}

bool Job::save()
{
    // return in case the job is already started
    if(isStarted())
        return false;
    // build the JSON object for this job
    QJsonObject json;
    serializeToJSON(&json);
    // create the job directory
    QDir dir;
    if(!dir.mkpath(_url.toLocalFile()))
    {
        qCritical() << _name << ": unable to create the job directory";
        return false;
    }
    // open a file handler
    QDir jobDirectory(_url.toLocalFile());
    QFile jobFile(jobDirectory.filePath("job.json"));
    if(!jobFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        qWarning() << _name << ": unable to write the job descriptor file" << jobFile.fileName();
        return false;
    }
    // write & close the file handler
    QJsonDocument jsonDocument(json);
    jobFile.write(jsonDocument.toJson());
    jobFile.close();
    return true;
}

bool Job::start()
{
    // do not start a job we can't save
    if(!save())
        return false;
    // do not start an invalid job
    if(!isStartable())
        return false;
    // create the build directory
    QDir dir(_url.toLocalFile());
    if(!dir.mkpath("build"))
    {
        qCritical() << _name << ": unable to create the job directory";
        return false;
    }
    // define the program path
    QString startCommand = std::getenv("MESHROOM_START_COMMAND");
    if(startCommand.isEmpty())
        startCommand = QCoreApplication::applicationDirPath() + "/scripts/job_start.py";
    // and add command arguments
    QStringList arguments;
    arguments.append(_url.toLocalFile() + "/job.json");
    // then run the process
    QProcess process;
    process.setProgram(startCommand);
    process.setArguments(arguments);
    process.start();
    // wait for the end of the process
    if(!process.waitForFinished())
    {
        // remove the build directory in case of error
        qCritical() << _name << ": unable to start job";
        dir.cd("build");
        dir.removeRecursively();
        return false;
    }
    // and refresh the job status
    qInfo() << _name << ": job started";
    refresh();
    return true;
}

void Job::refresh()
{
    if(!isStarted())
    {
        model()->setData(_modelIndex, -1, JobModel::StatusRole);
        return;
    }
    QFileInfo fileInfo(_url.toLocalFile() + "/job.json");
    // define program path
    QString statusCommand = std::getenv("MESHROOM_STATUS_COMMAND");
    if(statusCommand.isEmpty())
        statusCommand = QCoreApplication::applicationDirPath() + "/scripts/job_status.py";
    // and command arguments
    QStringList arguments;
    arguments.append(fileInfo.absoluteFilePath());
    // configure & run
    QProcess process;
    QObject::connect(&process, SIGNAL(finished(int, QProcess::ExitStatus)), this,
                     SLOT(readProcessOutput(int, QProcess::ExitStatus)));
    process.setProgram(statusCommand);
    process.setArguments(arguments);
    process.start();
    if(!process.waitForFinished())
        qCritical() << _name << ": unable to update job status";
}

void Job::erase()
{
    QDir dir(_url.toLocalFile());
    if(dir.exists())
        dir.removeRecursively();
}

void Job::readProcessOutput(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess* process = qobject_cast<QProcess*>(QObject::sender());
    assert(process);
    // check exit status
    if(exitStatus != QProcess::NormalExit)
    {
        QString response(process->readAllStandardError());
        qCritical() << response;
        model()->setData(_modelIndex, 4, JobModel::StatusRole); // ERROR
        return;
    }
    // parse standard output as JSON
    QJsonParseError parseError;
    QString response(process->readAllStandardOutput());
    QJsonDocument jsondoc(QJsonDocument::fromJson(response.toUtf8(), &parseError));
    if(parseError.error != QJsonParseError::NoError)
    {
        qCritical() << _name << ": invalid response - parse error";
        model()->setData(_modelIndex, 4, JobModel::StatusRole); // ERROR
        return;
    }
    // retrieve & set job completion & status
    QJsonObject json = jsondoc.object();
    if(!json.contains("completion") || !json.contains("status"))
    {
        qCritical() << _name << ": invalid response - missing values";
        return;
    }
    model()->setData(_modelIndex, json["completion"].toDouble(), JobModel::CompletionRole);
    model()->setData(_modelIndex, json["status"].toInt(), JobModel::StatusRole);
}

void Job::selectThumbnail()
{
    if(!model())
        return;
    QModelIndex image0ID = _images->index(0, 0);
    model()->setData(_modelIndex, _images->data(image0ID, ResourceModel::UrlRole),
                   JobModel::ThumbnailRole);
}

bool Job::isStoredOnDisk()
{
    QDir dir(_url.toLocalFile());
    QFile file(dir.filePath("job.json"));
    return file.exists();
}

bool Job::isStartable()
{
    if(_images->rowCount() < 2)
    {
        qCritical() << _name << ": insufficient number of sources";
        return false;
    }
    return true;
}

bool Job::isStarted()
{
    QDir dir(_url.toLocalFile()+"/build");
    return (dir.exists() && isStoredOnDisk());
}

bool Job::isPairA(const QUrl& url)
{
    QModelIndex id;
    Step* step = nullptr;
    Attribute* initialPairAttribute = getAttribute(*this, "sfm", "initial_pair", id, &step);
    if(!step || !initialPairAttribute)
        return false;
    QVariantList pair = initialPairAttribute->value().toList();
    return (pair.count() > 0 && pair[0] == url.toLocalFile());
}

bool Job::isPairB(const QUrl& url)
{
    QModelIndex id;
    Step* step = nullptr;
    Attribute* initialPairAttribute = getAttribute(*this, "sfm", "initial_pair", id, &step);
    if(!step || !initialPairAttribute)
        return false;
    QVariantList pair = initialPairAttribute->value().toList();
    return (pair.count() > 1 && pair[1] == url.toLocalFile());
}

bool Job::isPairValid()
{
    QModelIndex id;
    Step* step = nullptr;
    Attribute* initialPairAttribute = getAttribute(*this, "sfm", "initial_pair", id, &step);
    if(!step || !initialPairAttribute)
        return false;
    QVariantList pair = initialPairAttribute->value().toList();
    return (pair.count() > 1 && QUrl::fromLocalFile(pair[0].toString()).isValid() &&
            QUrl::fromLocalFile(pair[1].toString()).isValid());
}

void Job::createDefaultGraph()
{
    // create feature detection step
    Step* step = new Step("feature_detection");
    Attribute* att = new Attribute();
    att->setType(2); // combo
    att->setKey("describerPreset");
    att->setName("quality");
    att->setValue("Normal");
    att->setOptions(QStringList({"Normal", "High", "Ultra"}));
    step->attributes()->addAttribute(att);
    _steps->addStep(step);
    // create meshing step
    step = new Step("meshing");
    att = new Attribute();
    att->setType(1); // slider
    att->setKey("scale");
    att->setName("meshing scale");
    att->setValue(2);
    att->setMin(1);
    att->setMax(10);
    att->setStep(1);
    step->attributes()->addAttribute(att);
    _steps->addStep(step);
    // create sfm step
    step = new Step("sfm");
    att = new Attribute();
    att->setType(3); // pair selector
    att->setKey("initial_pair");
    att->setName("initial pair");
    att->setValue(QStringList({"", ""}));
    step->attributes()->addAttribute(att);
    _steps->addStep(step);
}

void Job::serializeToJSON(QJsonObject* obj) const
{
    if(!obj)
        return;
    // build the paths object
    QJsonObject pathsObject;
    pathsObject["build"] = _url.toLocalFile() + "/build";
    pathsObject["match"] = _url.toLocalFile() + "/build/matches";
    // build the resources array
    QJsonArray resourceArray;
    for(size_t i = 0; i < _images->rowCount(); i++)
    {
        QModelIndex id = _images->index(i, 0);
        Resource* resource = _images->data(id, ResourceModel::ModelDataRole).value<Resource*>();
        if(resource)
            resource->serializeToJSON(&resourceArray);
    }
    // build the steps object
    QJsonObject stepsObject;
    for(size_t i = 0; i < _steps->rowCount(); i++)
    {
        QModelIndex id = _steps->index(i, 0);
        Step* step = _steps->data(id, StepModel::ModelDataRole).value<Step*>();
        if(step)
            step->serializeToJSON(&stepsObject);
    }
    // then fill the main JSON object
    obj->insert("date", QJsonValue::fromVariant(_date));
    obj->insert("user", _user);
    obj->insert("name", _name);
    obj->insert("paths", pathsObject);
    obj->insert("resources", resourceArray);
    obj->insert("steps", stepsObject);
}

void Job::deserializeFromJSON(const QJsonObject& obj)
{
    autoSaveOff();
    // read global job settings
    if(obj.contains("user"))
        _user = obj["user"].toString();
    if(obj.contains("name"))
        _name = obj["name"].toString();
    // read job ressources
    QJsonArray resourceArray = obj["resources"].toArray();
    QObjectList resources;
    for(int i = 0; i < resourceArray.count(); ++i)
    {
        Resource* r = new Resource(QUrl::fromLocalFile(resourceArray.at(i).toString()));
        _images->addResource(r);
    }
    // read job steps (and their attributes)
    QJsonObject stepsObject = obj["steps"].toObject();
    for(size_t i = 0; i < _steps->rowCount(); i++)
    {
        QModelIndex id = _steps->index(i, 0);
        Step* step = _steps->data(id, StepModel::ModelDataRole).value<Step*>();
        if(!step)
            continue;
        step->deserializeFromJSON(stepsObject);
    }
    autoSaveOn();
}

} // namespace
