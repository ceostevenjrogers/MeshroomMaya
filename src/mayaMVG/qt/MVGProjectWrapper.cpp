#include "mayaMVG/qt/MVGProjectWrapper.hpp"
#include "mayaMVG/version.hpp"
#include <QCoreApplication>
#include "mayaMVG/qt/MVGCameraWrapper.hpp"
#include "mayaMVG/qt/MVGMeshWrapper.hpp"
#include "mayaMVG/maya/MVGMayaUtil.hpp"
#include "mayaMVG/core/MVGLog.hpp"
#include "mayaMVG/maya/context/MVGContext.hpp"
#include "mayaMVG/maya/context/MVGMoveManipulator.hpp"
#include <maya/MQtUtil.h>

namespace mayaMVG
{

MVGProjectWrapper::MVGProjectWrapper()
{
    MVGPanelWrapper* leftPanel = new MVGPanelWrapper("mvgLPanel", "Left");
    MVGPanelWrapper* rightPanel = new MVGPanelWrapper("mvgRPanel", "Right");
    _panelList.append(leftPanel);
    _panelList.append(rightPanel);
    // Initialize currentContext
    MString context;
    MVGMayaUtil::getCurrentContext(context);
    _currentContext = context.asChar();
    _editMode = MVGContext::eEditModeCreate;
    _moveMode = MVGMoveManipulator::eMoveModeNViewTriangulation;

    // Init unit map
    _unitMap[MDistance::kInches] = "in";
    _unitMap[MDistance::kFeet] = "ft";
    _unitMap[MDistance::kYards] = "yd";
    _unitMap[MDistance::kMiles] = "mi";
    _unitMap[MDistance::kMillimeters] = "mm";
    _unitMap[MDistance::kCentimeters] = "cm";
    _unitMap[MDistance::kKilometers] = "km";
    _unitMap[MDistance::kMeters] = "m";
    assert(_unitMap.size() == MDistance::kLast - 1); // First value is MDistance::kInvalid

    // Init _isProjectLoading
    _isProjectLoading = false;
}

MVGProjectWrapper::~MVGProjectWrapper()
{
}

const QString MVGProjectWrapper::getProjectDirectory() const
{
    if(!_project.isValid())
        return "";
    return QString(_project.getProjectDirectory().c_str());
}

const QString MVGProjectWrapper::getCurrentContext() const
{
    return _currentContext;
}

void MVGProjectWrapper::setCurrentContext(const QString& context)
{
    _currentContext = context;
    Q_EMIT currentContextChanged();
}

const QString MVGProjectWrapper::getCurrentUnit() const
{
    MDistance::Unit currentUnit = MDistance::uiUnit();
    return _unitMap[currentUnit];
}

const QString MVGProjectWrapper::getPluginVersion() const
{
    return MAYAMVG_VERSION;
}

void MVGProjectWrapper::setIsProjectLoading(const bool value)
{
    if(value == _isProjectLoading)
        return;
    _isProjectLoading = value;
    Q_EMIT isProjectLoadingChanged();
}

void MVGProjectWrapper::setProjectDirectory(const QString& directory)
{
    if(_project.isValid())
        _project.setProjectDirectory(directory.toStdString());
    Q_EMIT projectDirectoryChanged();
}

QString MVGProjectWrapper::openFileDialog() const
{
    MString directoryPath;
    MVGMayaUtil::openFileDialog(directoryPath);

    return MQtUtil::toQString(directoryPath);
}

void MVGProjectWrapper::scaleScene(const double scaleSize) const
{
    double internalUnit = MDistance::uiToInternal(scaleSize);
    if(!_project.scaleScene(internalUnit))
        LOG_ERROR("Cannot scale scene")
}

void MVGProjectWrapper::activeSelectionContext() const
{
    MVGMayaUtil::activeSelectionContext();
}

void MVGProjectWrapper::setCreationMode()
{
    MVGMayaUtil::setCreationMode();
}

void MVGProjectWrapper::setTriangulationMode()
{
    MVGMayaUtil::setTriangulationMode();
}

void MVGProjectWrapper::setPointCloudMode()
{
    MVGMayaUtil::setPointCloudMode();
}

void MVGProjectWrapper::setAdjacentPlaneMode()
{
    MVGMayaUtil::setAdjacentPlaneMode();
}

void MVGProjectWrapper::loadExistingProject()
{
    clear();
    std::vector<MVGProject> projects = MVGProject::list();
    if(projects.empty())
        return;
    _project = projects.front();
    reloadMVGCamerasFromMaya();
    reloadMVGMeshesFromMaya();
    Q_EMIT projectDirectoryChanged();
}

void MVGProjectWrapper::loadNewProject(const QString& projectDirectoryPath)
{
    // Cancel load
    if(projectDirectoryPath.isEmpty())
        return;

    clear();
    activeSelectionContext();
    if(!_project.isValid())
        _project = MVGProject::create(MVGProject::_PROJECT);
    setIsProjectLoading(true);

    // Loading project takes a lot of time.
    // So we ask Qt to process events (UI) to display the "Loading" status
    // before Maya will freeze the application.
    qApp->processEvents();

    if(!_project.load(projectDirectoryPath.toStdString()))
    {
        setProjectDirectory("");
        setIsProjectLoading(false);
        LOG_ERROR("An error occured when loading project.");
        return;
    }
    _project.setProjectDirectory(projectDirectoryPath.toStdString());
    Q_EMIT projectDirectoryChanged();
    setIsProjectLoading(false);

    reloadMVGCamerasFromMaya();
    reloadMVGMeshesFromMaya();

    // Loading image planes takes a lot of time.
    // So before loading image planes, we ask Qt to process events (UI)
    // before Maya will freeze the application.
    qApp->processEvents();

    // Select the two first cameras for the views
    if(_cameraList.size() > 1 && _panelList.count() > 1)
    {
        QList<MVGCameraWrapper*>& cameras = _cameraList.asQList<MVGCameraWrapper>();
        setCameraToView(cameras[0], static_cast<MVGPanelWrapper*>(_panelList.get(0))->getName());
        setCameraToView(cameras[1], static_cast<MVGPanelWrapper*>(_panelList.get(1))->getName());
    }
}

void MVGProjectWrapper::addCamerasToIHMSelection(const QStringList& selectedCameraNames,
                                                 bool center)
{
    // Reset old selection to false
    clearCameraSelection();
    // Set new selection to true
    for(QStringList::const_iterator it = selectedCameraNames.begin();
        it != selectedCameraNames.end(); ++it)
    {
        if(_camerasByName.count(it->toStdString()) == 0)
            continue;
        MVGCameraWrapper* camera = _camerasByName[it->toStdString()];
        camera->setIsSelected(true);
        _selectedCameras.append(*it);
        // Replace listView and set image in first viewort
        // TODO : let the user define in which viewport he wants to display the selected camera
        if(center && camera->getDagPathAsString() == selectedCameraNames[0])
        {
            setCameraToView(camera, static_cast<MVGPanelWrapper*>(_panelList.get(0))->getName());
            Q_EMIT centerCameraListByIndex(_cameraList.indexOf(camera));
        }
    }
}

void MVGProjectWrapper::addCamerasToMayaSelection(const QStringList& cameraNames) const
{
    if(!_project.isValid())
        return;
    std::vector<std::string> cameras;
    for(QStringList::const_iterator it = cameraNames.begin(); it != cameraNames.end(); ++it)
        cameras.push_back(it->toStdString());
    _project.selectCameras(cameras);
}

void MVGProjectWrapper::addMeshesToIHMSelection(const QStringList& selectedMeshPaths, bool center)
{
    // Reset old selection to false
    clearMeshSelection();
    // Set new selection to true
    for(QStringList::const_iterator it = selectedMeshPaths.begin(); it != selectedMeshPaths.end();
        ++it)
    {
        if(_meshesByName.count(it->toStdString()) == 0)
            continue;
        MVGMeshWrapper* mesh = _meshesByName[it->toStdString()];
        mesh->setIsSelected(true);
        _selectedMeshes.append(mesh->getDagPathAsString());
        // Replace listView
        // TODO : let the user define in which viewport he wants to display the selected camera
        if(center && mesh->getDagPathAsString() == selectedMeshPaths[0])
            Q_EMIT centerMeshListByIndex(_meshesList.indexOf(mesh));
    }
}

void MVGProjectWrapper::addMeshesToMayaSelection(const QStringList& meshesPath) const
{
    if(!_project.isValid())
        return;
    std::vector<std::string> meshes;
    for(QStringList::const_iterator it = meshesPath.begin(); it != meshesPath.end(); ++it)
        meshes.push_back(it->toStdString());
    _project.selectMeshes(meshes);
}

void MVGProjectWrapper::setCameraToView(QObject* camera, const QString& viewName)
{
    MVGCameraWrapper* cameraWrapper = static_cast<MVGCameraWrapper*>(camera);

    // Push command
    _project.pushLoadCurrentImagePlaneCommand(viewName.toStdString());
    // Set UI
    foreach(MVGCameraWrapper* c, _cameraList.asQList<MVGCameraWrapper>())
        c->setInView(viewName, false);
    MVGCameraWrapper* cam = qobject_cast<MVGCameraWrapper*>(camera);
    cam->setInView(viewName, true);
    // Update active camera
    _activeCameraNameByView[viewName.toStdString()] =
        cameraWrapper->getDagPathAsString().toStdString();
}

void MVGProjectWrapper::setCamerasNear(const double near)
{
    // TODO : undoable ?
    for(std::map<std::string, MVGCameraWrapper*>::const_iterator it = _camerasByName.begin();
        it != _camerasByName.end(); ++it)
        it->second->getCamera().setNear(near);
}
void MVGProjectWrapper::setCamerasFar(const double far)
{
    // TODO : undoable ?
    for(std::map<std::string, MVGCameraWrapper*>::const_iterator it = _camerasByName.begin();
        it != _camerasByName.end(); ++it)
        it->second->getCamera().setFar(far);
}

void MVGProjectWrapper::setCameraLocatorScale(const double scale)
{
    // TODO : undoable ?
    for(std::map<std::string, MVGCameraWrapper*>::const_iterator it = _camerasByName.begin();
        it != _camerasByName.end(); ++it)
        it->second->getCamera().setLocatorScale(scale);
}

void MVGProjectWrapper::clear()
{
    _cameraList.clear();
    _camerasByName.clear();
    _activeCameraNameByView.clear();
    _selectedCameras.clear();
    _meshesList.clear();
    _meshesByName.clear();
    _selectedMeshes.clear();
    Q_EMIT projectDirectoryChanged();
}

void MVGProjectWrapper::clearImageCache()
{
    _project.clearImageCache();
}

void MVGProjectWrapper::clearCameraSelection()
{
    for(QStringList::const_iterator it = _selectedCameras.begin(); it != _selectedCameras.end();
        ++it)
    {
        std::map<std::string, MVGCameraWrapper*>::const_iterator foundIt =
            _camerasByName.find(it->toStdString());
        if(foundIt != _camerasByName.end())
            foundIt->second->setIsSelected(false);
    }
    _selectedCameras.clear();
}

void MVGProjectWrapper::clearMeshSelection()
{
    for(QStringList::const_iterator it = _selectedMeshes.begin(); it != _selectedMeshes.end(); ++it)
    {
        std::map<std::string, MVGMeshWrapper*>::const_iterator foundIt =
            _meshesByName.find(it->toStdString());
        if(foundIt != _meshesByName.end())
            foundIt->second->setIsSelected(false);
    }
    _selectedMeshes.clear();
}

void MVGProjectWrapper::removeCameraFromUI(MDagPath& cameraPath)
{
    MVGCamera camera(cameraPath);
    if(!camera.isValid())
        return;

    for(int i = 0; i < _cameraList.count(); ++i)
    {
        MVGCameraWrapper* cameraWrapper = static_cast<MVGCameraWrapper*>(_cameraList.at(i));
        if(!cameraWrapper)
            continue;
        if(cameraWrapper->getCamera().getName() != camera.getName())
            continue;
        MDagPath leftCameraPath, rightCameraPath;
        MVGMayaUtil::getCameraInView(leftCameraPath, "mvgLPanel");
        leftCameraPath.extendToShape();
        if(leftCameraPath.fullPathName() == cameraPath.fullPathName())
            MVGMayaUtil::clearCameraInView("mvgLPanel");
        MVGMayaUtil::getCameraInView(rightCameraPath, "mvgRPanel");
        rightCameraPath.extendToShape();
        if(rightCameraPath.fullPathName() == cameraPath.fullPathName())
            MVGMayaUtil::clearCameraInView("mvgRPanel");
        // Remove cameraWrapper
        _cameraList.removeAt(i);
    }
}
void MVGProjectWrapper::addMeshToUI(const MDagPath& meshPath)
{
    MVGMesh mesh(meshPath);

    MVGMeshWrapper* meshWrapper = new MVGMeshWrapper(mesh);
    _meshesList.append(meshWrapper);
}

void MVGProjectWrapper::removeMeshFromUI(const MDagPath& meshPath)
{
    MVGMesh mesh(meshPath);
    if(!meshPath.isValid())
        return;

    for(int i = 0; i < _meshesList.count(); ++i)
    {
        MVGMeshWrapper* meshWraper = static_cast<MVGMeshWrapper*>(_meshesList.at(i));
        if(!meshWraper)
            continue;
        if(meshWraper->getMesh().getName() != mesh.getName())
            continue;
        _meshesList.removeAt(i);
    }
}

void MVGProjectWrapper::emitCurrentUnitChanged()
{
    Q_EMIT currentUnitChanged();
}
    
void MVGProjectWrapper::setEditMode(const int mode)
{
    if(_editMode == mode)
        return;
    _editMode = mode;
    Q_EMIT editModeChanged();
}

void MVGProjectWrapper::setMoveMode(const int mode)
{
    if(_moveMode == mode)
        return;
    _moveMode = mode;
    Q_EMIT moveModeChanged();
}

void MVGProjectWrapper::reloadMVGCamerasFromMaya()
{
    _cameraList.clear();
    _camerasByName.clear();
    _activeCameraNameByView.clear();

    const std::vector<MVGCamera>& cameraList = MVGCamera::getCameras();
    std::vector<MVGCamera>::const_iterator it = cameraList.begin();
    for(; it != cameraList.end(); ++it)
    {
        MVGCameraWrapper* cameraWrapper = new MVGCameraWrapper(*it);
        _cameraList.append(cameraWrapper);
        _camerasByName[it->getDagPathAsString()] = cameraWrapper;
    }
    // TODO : Camera selection
}

void MVGProjectWrapper::reloadMVGMeshesFromMaya()
{
    _meshesList.clear();
    const std::vector<MVGMesh>& meshList = MVGMesh::listAllMeshes();
    std::vector<MVGMesh>::const_iterator it = meshList.begin();
    for(; it != meshList.end(); ++it)
    {
        MVGMeshWrapper* meshWrapper = new MVGMeshWrapper(*it);
        _meshesList.append(meshWrapper);
        _meshesByName[it->getDagPathAsString()] = meshWrapper;
    }
}

} // namespace
