#include "mayaMVG/maya/context/MVGMoveManipulator.h"
#include "mayaMVG/maya/context/MVGContext.h"
#include "mayaMVG/maya/context/MVGDrawUtil.h"
#include "mayaMVG/maya/cmd/MVGEditCmd.h"
#include "mayaMVG/maya/MVGMayaUtil.h"
#include "mayaMVG/core/MVGGeometryUtil.h"
#include "mayaMVG/core/MVGMesh.h"
#include "mayaMVG/qt/MVGUserLog.h"
// #include "openMVG/multiview/triangulation.hpp"

using namespace mayaMVG;

MTypeId MVGMoveManipulator::_id(0x99222); // FIXME /!\

MVGMoveManipulator::MVGMoveManipulator()
    : _moveState(eMoveNone)
    , _moveInPlaneColor(0.f, 0.f, 1.f)      // Blue
    , _moveRecomputeColor(0.f, 1.f, 1.f)    // Cyan
    , _triangulateColor(0.9f, 0.5f, 0.4f)   // Orange
    , _faceColor(1.f, 1.f, 1.f)             // White
    , _noMoveColor(1.f, 0.f, 0.f)           // Red
    , _cursorColor(0.f, 0.f, 0.f)           // Black
    , _manipUtil(NULL)

{
}

MVGMoveManipulator::~MVGMoveManipulator()
{
}

void * MVGMoveManipulator::creator()
{
	return new MVGMoveManipulator();
}

MStatus MVGMoveManipulator::initialize()
{
	return MS::kSuccess;
}

void MVGMoveManipulator::postConstructor()
{
	registerForMouseMove();
}

void MVGMoveManipulator::draw(M3dView & view, const MDagPath & path,
                               M3dView::DisplayStyle style, M3dView::DisplayStatus dispStatus)
{
	view.beginGL();

    // CLEAN the input maya OpenGL State
    glDisable(GL_POLYGON_STIPPLE);
    glDisable(GL_LINE_STIPPLE);
    // Enable Alpha
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

	// enable GL picking, this will call manipulator::doPress/doRelease 
	MGLuint glPickableItem;
	glFirstHandle(glPickableItem);
	colorAndName(view, glPickableItem, true, mainColor());
		
	// 3D drawing
	_manipUtil->drawPreview3D();
	
	// starts 2D drawing 
	MVGDrawUtil::begin2DDrawing(view);
    MPoint center(0, 0);
    MVGDrawUtil::drawCircle2D(center, _cursorColor, 1, 5); // needed - FIXME

	// retrieve display data
	MVGManipulatorUtil::DisplayData* data = NULL;
	if(_manipUtil)
		data = _manipUtil->getDisplayData(view);
	// stop drawing in case of no data or not the active view
	if(!data || !MVGMayaUtil::isActiveView(view) || !MVGMayaUtil::isMVGView(view)) {
		MVGDrawUtil::end2DDrawing();
		view.endGL();
		return;
	}
	
	// update mouse coordinates & get it in camera space
	short mousex, mousey;
	MPoint mousePoint = updateMouse(view, mousex, mousey);

	// draw 
	drawCursor(mousex, mousey);
	drawIntersections(view);
    Qt::KeyboardModifiers modifiers = QApplication::keyboardModifiers();
    if(!(modifiers & Qt::ControlModifier) && !(modifiers & Qt::ShiftModifier)) {
        drawTriangulation(view, data, mousex, mousey);
    }

	MVGDrawUtil::end2DDrawing();
    glDisable(GL_BLEND);
	view.endGL();
}

MStatus MVGMoveManipulator::doPress(M3dView& view)
{
	Qt::MouseButtons mouseButtons = QApplication::mouseButtons();
	if(!(mouseButtons & Qt::LeftButton))
		return MS::kFailure;

	MVGManipulatorUtil::DisplayData* data = NULL;
	if(_manipUtil)
		data = _manipUtil->getDisplayData(view);
	if(!data)
		return MS::kFailure;
    
	short mousex, mousey;
	MPoint mousePoint = updateMouse(view, mousex, mousey);
	MVGManipulatorUtil::IntersectionData& intersectionData = _manipUtil->intersectionData();	
	_manipUtil->updateIntersectionState(view, data, mousex, mousey);
    
    Qt::KeyboardModifiers modifiers = QApplication::keyboardModifiers();
	switch(_manipUtil->intersectionState())
	{
		case MVGManipulatorUtil::eIntersectionNone:
			break;
		case MVGManipulatorUtil::eIntersectionPoint:
		{	
            if(intersectionData.pointIndex < 0)
                break;
			if((modifiers & Qt::ShiftModifier) || (modifiers & Qt::ControlModifier))
			{
                // Update face informations
                MVGMesh mesh(intersectionData.meshName);
                intersectionData.facePointIndexes.clear();
                MIntArray connectedFaceIndex = mesh.getConnectedFacesToVertex(intersectionData.pointIndex);
                if(connectedFaceIndex.length() > 0)
                    intersectionData.facePointIndexes = mesh.getFaceVertices(connectedFaceIndex[0]);						
			}
			_moveState = eMovePoint;
			break;
		}
        case MVGManipulatorUtil::eIntersectionEdge:			
            if(_manipUtil->computeEdgeIntersectionData(view, data, mousePoint))
                _moveState = eMoveEdge;
            break;
	}
	
	return MPxManipulatorNode::doPress(view);
}

MStatus MVGMoveManipulator::doRelease(M3dView& view)
{	
	MVGManipulatorUtil::DisplayData* data = NULL;
	if(_manipUtil)
		data = _manipUtil->getDisplayData(view);
	if(!data)
		return MS::kFailure;
	
	short mousex, mousey;
	MPoint mousePoint = updateMouse(view, mousex, mousey);
	
	MVGManipulatorUtil::IntersectionData& intersectionData = _manipUtil->intersectionData();
    Qt::KeyboardModifiers modifiers = QApplication::keyboardModifiers();
	switch(_moveState) {
		case eMoveNone:
			break;
		case eMovePoint:
		{
			if((modifiers & Qt::ShiftModifier) || (modifiers & Qt::ControlModifier))
			{           
                if(_manipUtil->previewFace3D().length() == 0)
                    break;
                MDagPath meshPath;
                MVGMayaUtil::getDagPathByName(intersectionData.meshName.c_str(), meshPath);
                _manipUtil->addUpdateFaceCommand(meshPath, _manipUtil->previewFace3D(), intersectionData.facePointIndexes);
                _manipUtil->previewFace3D().clear();
                intersectionData.facePointIndexes.clear();
            }
            else {
                if(intersectionData.pointIndex < 0)
                    break;
                MPoint triangulatedPoint;
                if(!triangulate(view, intersectionData, mousePoint, triangulatedPoint))
                    break;
                MDagPath meshPath;
                MVGMayaUtil::getDagPathByName(intersectionData.meshName.c_str(), meshPath);
                MPointArray newPoints;
                newPoints.append(triangulatedPoint);
                MIntArray indexes;
                indexes.append(intersectionData.pointIndex);
                _manipUtil->addUpdateFaceCommand(meshPath, newPoints, indexes);
                intersectionData.facePointIndexes.clear();
            }
			break;
		}		
		case eMoveEdge: 
		{
            if(intersectionData.edgePointIndexes.length() == 0)
                break;
            if((modifiers & Qt::ShiftModifier) || (modifiers & Qt::ControlModifier))
			{ 
                if(_manipUtil->previewFace3D().length() == 0)
                    break;

                MDagPath meshPath;
                MVGMayaUtil::getDagPathByName(intersectionData.meshName.c_str(), meshPath);

                MPointArray edgePoints;
                edgePoints.append(_manipUtil->previewFace3D()[2]);
                edgePoints.append(_manipUtil->previewFace3D()[3]);
                _manipUtil->addUpdateFaceCommand(meshPath, edgePoints, intersectionData.edgePointIndexes);
                _manipUtil->previewFace3D().clear();
                intersectionData.facePointIndexes.clear();
            }
			else {
                MPointArray triangulatedPoints;
                triangulateEdge(view, intersectionData, mousePoint, triangulatedPoints);
                MDagPath meshPath;
                MVGMayaUtil::getDagPathByName(intersectionData.meshName.c_str(), meshPath);
                _manipUtil->addUpdateFaceCommand(meshPath, triangulatedPoints, intersectionData.edgePointIndexes);
                intersectionData.facePointIndexes.clear();
            }
			break;
		}
	}

	_moveState = eMoveNone;
	return MPxManipulatorNode::doRelease(view);
}

MStatus MVGMoveManipulator::doMove(M3dView& view, bool& refresh)
{	
	refresh = true;
	
	MVGManipulatorUtil::DisplayData* data = NULL;
	if(_manipUtil)
		data = _manipUtil->getDisplayData(view);
	if(!data)
		return MS::kFailure;

	short mousex, mousey;
	mousePosition(mousex, mousey);

	if(data->allPoints2D.size() > 0)
		_manipUtil->updateIntersectionState(view, data, mousex, mousey);
	return MPxManipulatorNode::doMove(view, refresh);
}

MStatus MVGMoveManipulator::doDrag(M3dView& view)
{		
	MVGManipulatorUtil::DisplayData* data = NULL;
	if(_manipUtil)
		data = _manipUtil->getDisplayData(view);
	if(!data)
		return MS::kFailure;
	
	short mousex, mousey;
	MPoint mousePoint = updateMouse(view, mousex, mousey);
	
	MVGManipulatorUtil::IntersectionData& intersectionData = _manipUtil->intersectionData();
	std::vector<MVGManipulatorUtil::MVGPoint2D>& meshPoints = data->allPoints2D[intersectionData.meshName];
    
    Qt::KeyboardModifiers modifiers = QApplication::keyboardModifiers();
	switch(_moveState) {
		case eMoveNone:
			break;
		case eMovePoint: {
            if(intersectionData.pointIndex < 0 || meshPoints.size() < intersectionData.pointIndex)
                break;
            if(modifiers & Qt::ControlModifier) {
                if(meshPoints[intersectionData.pointIndex].movableState >= MVGManipulatorUtil::eMovableInSamePlane)
                    computeTmpFaceOnMovePoint(view, data, mousePoint);
            } else if(modifiers & Qt::ShiftModifier) {
                if(meshPoints[intersectionData.pointIndex].movableState == MVGManipulatorUtil::eMovableRecompute)
                    computeTmpFaceOnMovePoint(view, data, mousePoint, true);
            } else {
                _manipUtil->previewFace3D().clear();
                _manipUtil->intersectionState() = MVGManipulatorUtil::eIntersectionNone;
            }
			break;
		}
		case eMoveEdge: {
            if(intersectionData.edgePointIndexes.length() == 0 || meshPoints.size() < intersectionData.edgePointIndexes[0] || meshPoints.size() < intersectionData.edgePointIndexes[1])
                break;
            if(modifiers & Qt::ControlModifier) {
                if(meshPoints[intersectionData.edgePointIndexes[0]].movableState >= MVGManipulatorUtil::eMovableInSamePlane
                    && meshPoints[intersectionData.edgePointIndexes[1]].movableState >= MVGManipulatorUtil::eMovableInSamePlane)
                    computeTmpFaceOnMoveEdge(view, data, mousePoint);
            }
            else if(modifiers & Qt::ShiftModifier) {
                if(meshPoints[intersectionData.edgePointIndexes[0]].movableState >= MVGManipulatorUtil::eMovableInSamePlane
                    && meshPoints[intersectionData.edgePointIndexes[1]].movableState >= MVGManipulatorUtil::eMovableInSamePlane)
                    computeTmpFaceOnMoveEdge(view, data, mousePoint, true);
            }
            else {
                _manipUtil->previewFace3D().clear();
                _manipUtil->intersectionState() = MVGManipulatorUtil::eIntersectionNone;
            }
			break;
		}
	}
	
	return MPxManipulatorNode::doDrag(view);
}

void MVGMoveManipulator::preDrawUI(const M3dView& view)
{
}


MPoint MVGMoveManipulator::updateMouse(M3dView& view, short& mousex, short& mousey)
{
	mousePosition(mousex, mousey);
	MPoint mousePointInCameraCoord;
	MVGGeometryUtil::viewToCamera(view, mousex, mousey, mousePointInCameraCoord);
	return mousePointInCameraCoord;
}

void MVGMoveManipulator::drawCursor(const float mousex, const float mousey)
{
	MVGDrawUtil::drawArrowsCursor(mousex, mousey, _cursorColor);
    Qt::KeyboardModifiers modifiers = QApplication::keyboardModifiers();
    if(modifiers & Qt::ControlModifier)
        MVGDrawUtil::drawPlaneItem(mousex + 12, mousey + 10, _moveInPlaneColor);
    else if(modifiers & Qt::ShiftModifier)
        MVGDrawUtil::drawPointCloudItem(mousex + 10, mousey + 10, _moveRecomputeColor);
    else
        MVGDrawUtil::drawFullCross(mousex + 10, mousey + 10, 5, 1, _triangulateColor);
}
		
void MVGMoveManipulator::drawIntersections(M3dView& view)
{
	MVGManipulatorUtil::DisplayData* data = _manipUtil->getDisplayData(view);
	if(!data || data->allPoints2D.empty())
		return;

	MVGManipulatorUtil::IntersectionData& intersectionData = _manipUtil->intersectionData();
	std::vector<MVGManipulatorUtil::MVGPoint2D>& meshPoints = data->allPoints2D[intersectionData.meshName];
	
    Qt::KeyboardModifiers modifiers = QApplication::keyboardModifiers();
	switch(_manipUtil->intersectionState())
	{
		case MVGManipulatorUtil::eIntersectionPoint:
		{
            if(intersectionData.pointIndex < 0 || meshPoints.size() < intersectionData.pointIndex)
                return;
			MVGManipulatorUtil::EPointState movableState = meshPoints[intersectionData.pointIndex].movableState;
            MPoint point = MVGGeometryUtil::worldToView(view, meshPoints[intersectionData.pointIndex].point3D);
            if(modifiers & Qt::ControlModifier)
            {
                if(movableState >= MVGManipulatorUtil::eMovableInSamePlane)
                    MVGDrawUtil::drawCircle2D(point, _moveInPlaneColor, POINT_RADIUS, 30);
                else
                    MVGDrawUtil::drawCircle2D(point, _noMoveColor, POINT_RADIUS, 30);
            }
            else if(modifiers & Qt::ShiftModifier)
            {
                if(movableState == MVGManipulatorUtil::eMovableRecompute)
                    MVGDrawUtil::drawCircle2D(point, _moveRecomputeColor, POINT_RADIUS, 30);
                else
                    MVGDrawUtil::drawCircle2D(point, _noMoveColor, POINT_RADIUS, 30);
            }
            else
            {
                MVGDrawUtil::drawCircle2D(point, _triangulateColor, POINT_RADIUS, 30);
            }
			break;
		}
		case MVGManipulatorUtil::eIntersectionEdge:	{
            if(intersectionData.edgePointIndexes.length() == 0 || meshPoints.size() < intersectionData.edgePointIndexes[0] || meshPoints.size() < intersectionData.edgePointIndexes[1])
                return;
			std::vector<MVGManipulatorUtil::EPointState> movableStates;
			movableStates.push_back(meshPoints[intersectionData.edgePointIndexes[0]].movableState);
			movableStates.push_back(meshPoints[intersectionData.edgePointIndexes[1]].movableState);
            MPoint viewPoint1 = MVGGeometryUtil::worldToView(view,  meshPoints[intersectionData.edgePointIndexes[0]].point3D);
            MPoint viewPoint2 = MVGGeometryUtil::worldToView(view, meshPoints[intersectionData.edgePointIndexes[1]].point3D);

            if(modifiers & Qt::ControlModifier)
            {
                if((movableStates[0] >= MVGManipulatorUtil::eMovableInSamePlane) && (movableStates[1] >= MVGManipulatorUtil::eMovableInSamePlane))
                    MVGDrawUtil::drawLine2D(viewPoint1, viewPoint2, _moveInPlaneColor);
                else
                    MVGDrawUtil::drawLine2D(viewPoint1, viewPoint2, _noMoveColor);
            }
            else if(modifiers & Qt::ShiftModifier)
            {
                if((movableStates[0] >= MVGManipulatorUtil::eMovableInSamePlane) && (movableStates[1] >= MVGManipulatorUtil::eMovableInSamePlane))
                    MVGDrawUtil::drawLine2D(viewPoint1, viewPoint2, _moveRecomputeColor);
                else
                    MVGDrawUtil::drawLine2D(viewPoint1, viewPoint2, _noMoveColor);
            }
            else
            {
                MVGDrawUtil::drawLine2D(viewPoint1, viewPoint2, _triangulateColor);
            }
			break;
        }
	}
}

void MVGMoveManipulator::drawTriangulation(M3dView& view, MVGManipulatorUtil::DisplayData* data, const float mousex, const float mousey)
{
    switch(_moveState) {
        case eMoveNone:
            break;
        case eMovePoint: {
            if(_manipUtil->intersectionData().pointIndex < 0)
                break;
            MPoint& point3D = data->allPoints2D[_manipUtil->intersectionData().meshName].at(_manipUtil->intersectionData().pointIndex).point3D;
            MPoint viewPoint = MVGGeometryUtil::worldToView(view, point3D);
            MPoint mouseView(mousex, mousey);
            MVGDrawUtil::drawLine2D(viewPoint, mouseView, _triangulateColor, 1.5f, 1.f, true);
            break;
        }
        case eMoveEdge: {
            MPoint mousePoint;
            MVGGeometryUtil::viewToCamera(view, mousex, mousey, mousePoint);
            MPoint viewPoint1;
            MPoint viewPoint2;
            MPoint P1 = mousePoint + _manipUtil->intersectionData().edgeRatio * _manipUtil->intersectionData().edgeHeight2D;
            MPoint P0 = mousePoint  - (1 - _manipUtil->intersectionData().edgeRatio) * _manipUtil->intersectionData().edgeHeight2D;
            MVGGeometryUtil::cameraToView(view, P1, viewPoint1);
            MVGGeometryUtil::cameraToView(view, P0, viewPoint2);
            MVGDrawUtil::drawLine2D(viewPoint1, viewPoint2, _triangulateColor);
            break;
        }
    }
}

void MVGMoveManipulator::computeTmpFaceOnMovePoint(M3dView& view, MVGManipulatorUtil::DisplayData* data, const MPoint& mousePoint, bool recomputePlane)
{
	// 
	MVGManipulatorUtil::IntersectionData& intersectionData = _manipUtil->intersectionData();
	std::vector<MVGManipulatorUtil::MVGPoint2D>& meshPoints2D = data->allPoints2D[intersectionData.meshName];
	MIntArray faceVerticesId = intersectionData.facePointIndexes;
	MPointArray& previewFace3D = _manipUtil->previewFace3D();
	
    if(intersectionData.pointIndex < 0)
        return;
    for(int i = 0; i < faceVerticesId.length(); ++i) {
        if(meshPoints2D.size() < faceVerticesId[i])
            return;
    }
	if(recomputePlane) {
		MPointArray previewPoints2D;
		for(int i = 0; i < faceVerticesId.length(); ++i) {
			if(intersectionData.pointIndex == faceVerticesId[i])
				previewPoints2D.append(mousePoint);
			else
				previewPoints2D.append(meshPoints2D[faceVerticesId[i]].projectedPoint3D);
		}
		// Compute face
		previewFace3D.clear();
		MVGGeometryUtil::projectFace2D(view, previewFace3D, data->camera, previewPoints2D);
	} else {
		// Fill previewFace3D
		MPointArray facePoints3D;
		for(int i = 0; i < faceVerticesId.length(); ++i)
			facePoints3D.append(meshPoints2D[faceVerticesId[i]].point3D);
		MPoint movedPoint;
		PlaneKernel::Model model;
		MVGGeometryUtil::computePlane(facePoints3D, model);
		MVGGeometryUtil::projectPointOnPlane(mousePoint, view, model, data->camera, movedPoint);
		previewFace3D.clear();
		previewFace3D.setLength(faceVerticesId.length());
		for(int i = 0; i < faceVerticesId.length(); ++i) {
			if(intersectionData.pointIndex == faceVerticesId[i])
				previewFace3D[i] = movedPoint;
			else
				previewFace3D[i] = facePoints3D[i];
		}		
	}
}


void MVGMoveManipulator::computeTmpFaceOnMoveEdge(M3dView& view, MVGManipulatorUtil::DisplayData* data, const MPoint& mousePoint, bool recompute)
{
    MVGManipulatorUtil::IntersectionData& intersectionData = _manipUtil->intersectionData();
    std::vector<MVGManipulatorUtil::MVGPoint2D>& meshPoints2D = data->allPoints2D[intersectionData.meshName];
    MPointArray& previewFace3D = _manipUtil->previewFace3D();
    if(intersectionData.edgePointIndexes.length() == 0)
        return;
	MIntArray faceVerticesId = intersectionData.facePointIndexes;
	MIntArray edgeVerticesId = intersectionData.edgePointIndexes;
	MIntArray fixedVerticesId;
	for(int i = 0; i < faceVerticesId.length(); ++i) {
        if(meshPoints2D.size() < faceVerticesId[i])
            return;
		int found = false;
		for(int j = 0; j < edgeVerticesId.length(); ++j) {
			if(faceVerticesId[i] == edgeVerticesId[j])
				found = true;
		}
		if(!found)
			fixedVerticesId.append(faceVerticesId[i]);
	}

	// Switch order if necessary
	if(fixedVerticesId[0] == faceVerticesId[0]
		&& fixedVerticesId[1] == faceVerticesId[faceVerticesId.length() - 1]) {
		MIntArray tmp = fixedVerticesId;
		fixedVerticesId[0] = tmp[1];
		fixedVerticesId[1] = tmp[0];
	}
		
    if(meshPoints2D.size() < fixedVerticesId[0] || meshPoints2D.size() < fixedVerticesId[1])
            return;

	if(recompute) {
		MPointArray previewPoints2D;

		// First : fixed points
		previewPoints2D.append(meshPoints2D[fixedVerticesId[0]].projectedPoint3D);
		previewPoints2D.append(meshPoints2D[fixedVerticesId[1]].projectedPoint3D);

		// Then : mousePoints computed with egdeHeight and ratio
		MPoint P3 = mousePoint + intersectionData.edgeRatio * intersectionData.edgeHeight2D;
		previewPoints2D.append(P3);
		MPoint P4 = mousePoint  - (1 - intersectionData.edgeRatio) * intersectionData.edgeHeight2D;
		previewPoints2D.append(P4);

		// Only set the new points to keep a connected face
		previewFace3D.clear();
		MVGGeometryUtil::projectFace2D(view, previewFace3D, data->camera, previewPoints2D, -intersectionData.edgeHeight3D);

		// Check points order
		MVector AD =  previewFace3D[3] - previewFace3D[0];
		MVector BC =  previewFace3D[2] - previewFace3D[1];
		if(MVGGeometryUtil::edgesIntersection(previewFace3D[0], previewFace3D[1], AD, BC)) {
			MPointArray tmp = previewFace3D;
			previewFace3D[3] = tmp[2];
			previewFace3D[2] = tmp[3];
		}
	}
	else {
		// Fill previewFace3D
		MPointArray facePoints3D;
		for(int i = 0; i < faceVerticesId.length(); ++i) {
			facePoints3D.append(meshPoints2D[faceVerticesId[i]].point3D);
		}

		// Compute plane with old face points
		MPoint movedPoint;
		PlaneKernel::Model model;
		MVGGeometryUtil::computePlane(facePoints3D, model);

		previewFace3D.clear();
		previewFace3D.setLength(4);
		previewFace3D[0] = meshPoints2D[fixedVerticesId[0]].point3D;
		previewFace3D[1] = meshPoints2D[fixedVerticesId[1]].point3D;

		// Project new points on plane
		MPoint P3 = mousePoint + _manipUtil->intersectionData().edgeRatio * _manipUtil->intersectionData().edgeHeight2D;
		MVGGeometryUtil::projectPointOnPlane(P3, view, model, data->camera, movedPoint);
		previewFace3D[3] = movedPoint;

		// Keep 3D length
		MPoint lastPoint3D = movedPoint - _manipUtil->intersectionData().edgeHeight3D;
		MPoint lastPoint2D;
		MVGGeometryUtil::worldToCamera(view, lastPoint3D, lastPoint2D);
		MVGGeometryUtil::projectPointOnPlane(lastPoint2D, view, model, data->camera, movedPoint);
		previewFace3D[2] = movedPoint;

	}
}

bool MVGMoveManipulator::triangulate(M3dView& view, const MVGManipulatorUtil::IntersectionData& intersectionData, const MPoint& mousePointInCameraCoord, MPoint& resultPoint3D)
{
	if(_manipUtil->getCacheCount() == 1) {
		USER_WARNING("Can't triangulate with the same camera")
		return false;
	}

	// Points in camera coordinates
	MPointArray points2D;
	points2D.append(mousePointInCameraCoord);

    MDagPath cameraPath;
	view.getCamera(cameraPath);
	MVGManipulatorUtil::DisplayData* complementaryData = _manipUtil->getComplementaryDisplayData(view);
	if(!complementaryData)
		return false;

	std::vector<MVGManipulatorUtil::MVGPoint2D>& meshPoints2D = complementaryData->allPoints2D[intersectionData.meshName];
    if(intersectionData.pointIndex < 0 || meshPoints2D.size() < intersectionData.pointIndex)
        return false;
	points2D.append(meshPoints2D[intersectionData.pointIndex].projectedPoint3D);

	std::vector<MVGCamera> cameras;
	cameras.push_back(MVGCamera(cameraPath));
	cameras.push_back(complementaryData->camera);

	MVGGeometryUtil::triangulatePoint(points2D, cameras, resultPoint3D);
	return true;
}

bool MVGMoveManipulator::triangulateEdge(M3dView& view, const MVGManipulatorUtil::IntersectionData& intersectionData, const MPoint& mousePointInCameraCoord, MPointArray& resultPoint3D)
{
	if(_manipUtil->getCacheCount() == 1) {
		USER_WARNING("Can't triangulate with the same camera")
		return false;
	}

    // Edge points (moved)
    MPointArray array0, array1;
    MPoint P1 = mousePointInCameraCoord + intersectionData.edgeRatio * intersectionData.edgeHeight2D;
    array1.append(P1);
    MPoint P0 = mousePointInCameraCoord - (1 - intersectionData.edgeRatio) * intersectionData.edgeHeight2D;
    array0.append(P0);

    MDagPath cameraPath;
	view.getCamera(cameraPath);

    MVGManipulatorUtil::DisplayData* complementaryData = _manipUtil->getComplementaryDisplayData(view);
	if(!complementaryData)
		return false;

    // Edge points
	std::vector<MVGManipulatorUtil::MVGPoint2D>& meshPoints2D = complementaryData->allPoints2D[intersectionData.meshName];
    if(intersectionData.edgePointIndexes.length() == 0 || meshPoints2D.size() < intersectionData.edgePointIndexes[0] || meshPoints2D.size() < intersectionData.edgePointIndexes[1])
        return false;
    array0.append(meshPoints2D[intersectionData.edgePointIndexes[0]].projectedPoint3D);
    array1.append(meshPoints2D[intersectionData.edgePointIndexes[1]].projectedPoint3D);

	// MVGCameras
    std::vector<MVGCamera> cameras;
    cameras.push_back(MVGCamera(cameraPath));
    cameras.push_back(complementaryData->camera);
        
    MPoint result;
	MVGGeometryUtil::triangulatePoint(array0, cameras, result);
    resultPoint3D.append(result);
	MVGGeometryUtil::triangulatePoint(array1, cameras, result);
    resultPoint3D.append(result);

	return true;
}

void MVGMoveManipulator::drawUI(MHWRender::MUIDrawManager& drawManager, const MHWRender::MFrameContext&) const
{
	drawManager.beginDrawable();
	drawManager.setColor(MColor(1.0, 0.0, 0.0, 0.6));
	// TODO
	drawManager.endDrawable();
}
