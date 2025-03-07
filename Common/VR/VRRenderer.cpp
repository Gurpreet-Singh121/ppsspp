#include "VRBase.h"
#include "VRInput.h"
#include "VRRenderer.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>

XrView* projections;
XrPosef invViewTransform[2];
XrFrameState frameState = {};
GLboolean initialized = GL_FALSE;
GLboolean stageSupported = GL_FALSE;
int vrConfig[VR_CONFIG_MAX] = {};

XrVector3f hmdorientation;
XrVector3f hmdposition;

void VR_UpdateStageBounds(ovrApp* pappState) {
	XrExtent2Df stageBounds = {};

	XrResult result;
	OXR(result = xrGetReferenceSpaceBoundsRect(pappState->Session, XR_REFERENCE_SPACE_TYPE_STAGE, &stageBounds));
	if (result != XR_SUCCESS) {
		ALOGV("Stage bounds query failed: using small defaults");
		stageBounds.width = 1.0f;
		stageBounds.height = 1.0f;

		pappState->CurrentSpace = pappState->FakeStageSpace;
	}

	ALOGV("Stage bounds: width = %f, depth %f", stageBounds.width, stageBounds.height);
}

void VR_GetResolution(engine_t* engine, int *pWidth, int *pHeight) {
	static int width = 0;
	static int height = 0;

	if (engine) {
		// Enumerate the viewport configurations.
		uint32_t viewportConfigTypeCount = 0;
		OXR(xrEnumerateViewConfigurations(
				engine->appState.Instance, engine->appState.SystemId, 0, &viewportConfigTypeCount, NULL));

		XrViewConfigurationType* viewportConfigurationTypes =
				(XrViewConfigurationType*)malloc(viewportConfigTypeCount * sizeof(XrViewConfigurationType));

		OXR(xrEnumerateViewConfigurations(
				engine->appState.Instance,
				engine->appState.SystemId,
				viewportConfigTypeCount,
				&viewportConfigTypeCount,
				viewportConfigurationTypes));

		ALOGV("Available Viewport Configuration Types: %d", viewportConfigTypeCount);

		for (uint32_t i = 0; i < viewportConfigTypeCount; i++) {
			const XrViewConfigurationType viewportConfigType = viewportConfigurationTypes[i];

			ALOGV(
					"Viewport configuration type %d : %s",
					viewportConfigType,
					viewportConfigType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO ? "Selected" : "");

			XrViewConfigurationProperties viewportConfig;
			viewportConfig.type = XR_TYPE_VIEW_CONFIGURATION_PROPERTIES;
			OXR(xrGetViewConfigurationProperties(
					engine->appState.Instance, engine->appState.SystemId, viewportConfigType, &viewportConfig));
			ALOGV(
					"FovMutable=%s ConfigurationType %d",
					viewportConfig.fovMutable ? "true" : "false",
					viewportConfig.viewConfigurationType);

			uint32_t viewCount;
			OXR(xrEnumerateViewConfigurationViews(
					engine->appState.Instance, engine->appState.SystemId, viewportConfigType, 0, &viewCount, NULL));

			if (viewCount > 0) {
				XrViewConfigurationView* elements =
						(XrViewConfigurationView*)malloc(viewCount * sizeof(XrViewConfigurationView));

				for (uint32_t e = 0; e < viewCount; e++) {
					elements[e].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
					elements[e].next = NULL;
				}

				OXR(xrEnumerateViewConfigurationViews(
						engine->appState.Instance,
						engine->appState.SystemId,
						viewportConfigType,
						viewCount,
						&viewCount,
						elements));

				// Cache the view config properties for the selected config type.
				if (viewportConfigType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
					assert(viewCount == ovrMaxNumEyes);
					for (uint32_t e = 0; e < viewCount; e++) {
						engine->appState.ViewConfigurationView[e] = elements[e];
					}
				}

				free(elements);
			} else {
				ALOGE("Empty viewport configuration type: %d", viewCount);
			}
		}

		free(viewportConfigurationTypes);

		*pWidth = width = engine->appState.ViewConfigurationView[0].recommendedImageRectWidth;
		*pHeight = height = engine->appState.ViewConfigurationView[0].recommendedImageRectHeight;
	} else {
		//use cached values
		*pWidth = width;
		*pHeight = height;
	}
}

void VR_Recenter(engine_t* engine) {

	// Calculate recenter reference
	XrReferenceSpaceCreateInfo spaceCreateInfo = {};
	spaceCreateInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	spaceCreateInfo.poseInReferenceSpace.orientation.w = 1.0f;
	if (engine->appState.CurrentSpace != XR_NULL_HANDLE) {
		XrSpaceLocation loc = {};
		loc.type = XR_TYPE_SPACE_LOCATION;
		OXR(xrLocateSpace(engine->appState.HeadSpace, engine->appState.CurrentSpace, engine->predictedDisplayTime, &loc));
		hmdorientation = XrQuaternionf_ToEulerAngles(loc.pose.orientation);

		vrConfig[VR_CONFIG_RECENTER_YAW] += (int)hmdorientation.y;
		float recenterYaw = ToRadians((float)vrConfig[VR_CONFIG_RECENTER_YAW]);
		spaceCreateInfo.poseInReferenceSpace.orientation.x = 0;
		spaceCreateInfo.poseInReferenceSpace.orientation.y = sin(recenterYaw / 2);
		spaceCreateInfo.poseInReferenceSpace.orientation.z = 0;
		spaceCreateInfo.poseInReferenceSpace.orientation.w = cos(recenterYaw / 2);
	}

	// Delete previous space instances
	if (engine->appState.StageSpace != XR_NULL_HANDLE) {
		OXR(xrDestroySpace(engine->appState.StageSpace));
	}
	if (engine->appState.FakeStageSpace != XR_NULL_HANDLE) {
		OXR(xrDestroySpace(engine->appState.FakeStageSpace));
	}

	// Create a default stage space to use if SPACE_TYPE_STAGE is not
	// supported, or calls to xrGetReferenceSpaceBoundsRect fail.
	spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
#ifdef OPENXR_FLOOR_STAGE
	spaceCreateInfo.poseInReferenceSpace.position.y = -1.6750f;
#endif
	OXR(xrCreateReferenceSpace(engine->appState.Session, &spaceCreateInfo, &engine->appState.FakeStageSpace));
	ALOGV("Created fake stage space from local space with offset");
	engine->appState.CurrentSpace = engine->appState.FakeStageSpace;

	if (stageSupported) {
		spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
		spaceCreateInfo.poseInReferenceSpace.position.y = 0.0;
		OXR(xrCreateReferenceSpace(engine->appState.Session, &spaceCreateInfo, &engine->appState.StageSpace));
		ALOGV("Created stage space");
#ifdef OPENXR_FLOOR_STAGE
		engine->appState.CurrentSpace = engine->appState.StageSpace;
#endif
	}

	// Update menu orientation
	vrConfig[VR_CONFIG_MENU_PITCH] = (int)hmdorientation.x;
	vrConfig[VR_CONFIG_MENU_YAW] = 0;
}

void VR_InitRenderer( engine_t* engine, bool multiview ) {
	if (initialized) {
		VR_DestroyRenderer(engine);
	}

	int eyeW, eyeH;
	VR_GetResolution(engine, &eyeW, &eyeH);
	vrConfig[VR_CONFIG_VIEWPORT_WIDTH] = eyeW;
	vrConfig[VR_CONFIG_VIEWPORT_HEIGHT] = eyeH;

	// Get the viewport configuration info for the chosen viewport configuration type.
	engine->appState.ViewportConfig.type = XR_TYPE_VIEW_CONFIGURATION_PROPERTIES;
	OXR(xrGetViewConfigurationProperties(engine->appState.Instance, engine->appState.SystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, &engine->appState.ViewportConfig));

	uint32_t numOutputSpaces = 0;
	OXR(xrEnumerateReferenceSpaces(engine->appState.Session, 0, &numOutputSpaces, NULL));
	XrReferenceSpaceType* referenceSpaces = (XrReferenceSpaceType*)malloc(numOutputSpaces * sizeof(XrReferenceSpaceType));
	OXR(xrEnumerateReferenceSpaces(engine->appState.Session, numOutputSpaces, &numOutputSpaces, referenceSpaces));

	for (uint32_t i = 0; i < numOutputSpaces; i++) {
		if (referenceSpaces[i] == XR_REFERENCE_SPACE_TYPE_STAGE) {
			stageSupported = GL_TRUE;
			break;
		}
	}

	free(referenceSpaces);

	if (engine->appState.CurrentSpace == XR_NULL_HANDLE) {
		VR_Recenter(engine);
	}

	projections = (XrView*)(malloc(ovrMaxNumEyes * sizeof(XrView)));

	ovrRenderer_Create(
			engine->appState.Session,
			&engine->appState.Renderer,
			engine->appState.ViewConfigurationView[0].recommendedImageRectWidth,
			engine->appState.ViewConfigurationView[0].recommendedImageRectHeight,
			multiview);
	initialized = GL_TRUE;
}

void VR_DestroyRenderer( engine_t* engine ) {
	ovrRenderer_Destroy(&engine->appState.Renderer);
	free(projections);
	initialized = GL_FALSE;
}

void VR_ClearFrameBuffer( int width, int height) {
	glEnable( GL_SCISSOR_TEST );
	glViewport( 0, 0, width, height );

	glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );

	glScissor( 0, 0, width, height );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

	glScissor( 0, 0, 0, 0 );
	glDisable( GL_SCISSOR_TEST );
}

bool VR_InitFrame( engine_t* engine ) {
	GLboolean stageBoundsDirty = GL_TRUE;
	if (ovrApp_HandleXrEvents(&engine->appState)) {
		VR_Recenter(engine);
	}
	if (engine->appState.SessionActive == GL_FALSE) {
		return false;
	}

	if (stageBoundsDirty) {
		VR_UpdateStageBounds(&engine->appState);
		stageBoundsDirty = GL_FALSE;
	}

	// NOTE: OpenXR does not use the concept of frame indices. Instead,
	// XrWaitFrame returns the predicted display time.
	XrFrameWaitInfo waitFrameInfo = {};
	waitFrameInfo.type = XR_TYPE_FRAME_WAIT_INFO;
	waitFrameInfo.next = NULL;

	frameState.type = XR_TYPE_FRAME_STATE;
	frameState.next = NULL;

	OXR(xrWaitFrame(engine->appState.Session, &waitFrameInfo, &frameState));
	engine->predictedDisplayTime = frameState.predictedDisplayTime;
	if (!frameState.shouldRender) {
		return false;
	}

	// Get the HMD pose, predicted for the middle of the time period during which
	// the new eye images will be displayed. The number of frames predicted ahead
	// depends on the pipeline depth of the engine and the synthesis rate.
	// The better the prediction, the less black will be pulled in at the edges.
	XrFrameBeginInfo beginFrameDesc = {};
	beginFrameDesc.type = XR_TYPE_FRAME_BEGIN_INFO;
	beginFrameDesc.next = NULL;
	OXR(xrBeginFrame(engine->appState.Session, &beginFrameDesc));

	XrViewLocateInfo projectionInfo = {};
	projectionInfo.type = XR_TYPE_VIEW_LOCATE_INFO;
	projectionInfo.viewConfigurationType = engine->appState.ViewportConfig.viewConfigurationType;
	projectionInfo.displayTime = frameState.predictedDisplayTime;
	projectionInfo.space = engine->appState.CurrentSpace;

	XrViewState viewState = {XR_TYPE_VIEW_STATE, NULL};

	uint32_t projectionCapacityInput = ovrMaxNumEyes;
	uint32_t projectionCountOutput = projectionCapacityInput;

	OXR(xrLocateViews(
			engine->appState.Session,
			&projectionInfo,
			&viewState,
			projectionCapacityInput,
			&projectionCountOutput,
			projections));
	//

	for (int eye = 0; eye < ovrMaxNumEyes; eye++) {
		invViewTransform[eye] = projections[eye].pose;
	}

	// Update HMD and controllers
	hmdorientation = XrQuaternionf_ToEulerAngles(invViewTransform[0].orientation);
	hmdposition = invViewTransform[0].position;
	IN_VRInputFrame(engine);

	engine->appState.LayerCount = 0;
	memset(engine->appState.Layers, 0, sizeof(ovrCompositorLayer_Union) * ovrMaxLayerCount);
	return true;
}

void VR_BeginFrame( engine_t* engine, int fboIndex ) {
	vrConfig[VR_CONFIG_CURRENT_FBO] = fboIndex;
	ovrFramebuffer* frameBuffer = &engine->appState.Renderer.FrameBuffer[fboIndex];
	ovrFramebuffer_Acquire(frameBuffer);
	ovrFramebuffer_SetCurrent(frameBuffer);
	VR_ClearFrameBuffer(frameBuffer->ColorSwapChain.Width, frameBuffer->ColorSwapChain.Height);
}

void VR_EndFrame( engine_t* engine ) {

	int fboIndex = vrConfig[VR_CONFIG_CURRENT_FBO];

	// Clear the alpha channel, other way OpenXR would not transfer the framebuffer fully
	VR_BindFramebuffer(engine);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	glClearColor(0.0, 0.0, 0.0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	// Show mouse cursor
	int size = vrConfig[VR_CONFIG_MOUSE_SIZE];
	if ((vrConfig[VR_CONFIG_MODE] == VR_MODE_FLAT_SCREEN) && (size > 0)) {
		glEnable(GL_SCISSOR_TEST);
		glScissor(vrConfig[VR_CONFIG_MOUSE_X], vrConfig[VR_CONFIG_MOUSE_Y], size, size);
		glViewport(vrConfig[VR_CONFIG_MOUSE_X], vrConfig[VR_CONFIG_MOUSE_Y], size, size);
		glClearColor(1, 1, 1, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glDisable(GL_SCISSOR_TEST);
	}

	ovrFramebuffer* frameBuffer = &engine->appState.Renderer.FrameBuffer[fboIndex];
	//ovrFramebuffer_Resolve(frameBuffer);
	ovrFramebuffer_Release(frameBuffer);
	ovrFramebuffer_SetNone();
}

void VR_FinishFrame( engine_t* engine ) {

	int vrMode = vrConfig[VR_CONFIG_MODE];
	XrCompositionLayerProjectionView projection_layer_elements[2] = {};
	if ((vrMode == VR_MODE_MONO_6DOF) || (vrMode == VR_MODE_STEREO_6DOF)) {
		vrConfig[VR_CONFIG_MENU_YAW] = (int)hmdorientation.y;

		for (int eye = 0; eye < ovrMaxNumEyes; eye++) {
			int imageLayer = engine->appState.Renderer.Multiview ? eye : 0;
			ovrFramebuffer* frameBuffer = &engine->appState.Renderer.FrameBuffer[0];
			XrFovf fov = projections[eye].fov;
			if (vrMode == VR_MODE_MONO_6DOF) {
				fov = projections[0].fov;
			} else if (!engine->appState.Renderer.Multiview) {
				frameBuffer = &engine->appState.Renderer.FrameBuffer[eye];
			}

			memset(&projection_layer_elements[eye], 0, sizeof(XrCompositionLayerProjectionView));
			projection_layer_elements[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			projection_layer_elements[eye].pose = invViewTransform[eye];
			projection_layer_elements[eye].fov = fov;

			memset(&projection_layer_elements[eye].subImage, 0, sizeof(XrSwapchainSubImage));
			projection_layer_elements[eye].subImage.swapchain = frameBuffer->ColorSwapChain.Handle;
			projection_layer_elements[eye].subImage.imageRect.offset.x = 0;
			projection_layer_elements[eye].subImage.imageRect.offset.y = 0;
			projection_layer_elements[eye].subImage.imageRect.extent.width = frameBuffer->ColorSwapChain.Width;
			projection_layer_elements[eye].subImage.imageRect.extent.height = frameBuffer->ColorSwapChain.Height;
			projection_layer_elements[eye].subImage.imageArrayIndex = imageLayer;
		}

		XrCompositionLayerProjection projection_layer = {};
		projection_layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
		projection_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		projection_layer.layerFlags |= XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;
		projection_layer.space = engine->appState.CurrentSpace;
		projection_layer.viewCount = ovrMaxNumEyes;
		projection_layer.views = projection_layer_elements;

		engine->appState.Layers[engine->appState.LayerCount++].Projection = projection_layer;
	} else if (vrMode == VR_MODE_FLAT_SCREEN) {

		// Build the cylinder layer
		int width = engine->appState.Renderer.FrameBuffer[0].ColorSwapChain.Width;
		int height = engine->appState.Renderer.FrameBuffer[0].ColorSwapChain.Height;
		XrCompositionLayerCylinderKHR cylinder_layer = {};
		cylinder_layer.type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
		cylinder_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		cylinder_layer.space = engine->appState.CurrentSpace;
		cylinder_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		memset(&cylinder_layer.subImage, 0, sizeof(XrSwapchainSubImage));
		cylinder_layer.subImage.swapchain = engine->appState.Renderer.FrameBuffer[0].ColorSwapChain.Handle;
		cylinder_layer.subImage.imageRect.offset.x = 0;
		cylinder_layer.subImage.imageRect.offset.y = 0;
		cylinder_layer.subImage.imageRect.extent.width = width;
		cylinder_layer.subImage.imageRect.extent.height = height;
		cylinder_layer.subImage.imageArrayIndex = 0;
		float distance = (float)vrConfig[VR_CONFIG_CANVAS_DISTANCE];
		float menuPitch = ToRadians((float)vrConfig[VR_CONFIG_MENU_PITCH]);
		float menuYaw = ToRadians((float)vrConfig[VR_CONFIG_MENU_YAW]);
		XrVector3f pos = {
				invViewTransform[0].position.x - sin(menuYaw) * distance,
				invViewTransform[0].position.y,
				invViewTransform[0].position.z - cos(menuYaw) * distance
		};
		XrQuaternionf pitch = XrQuaternionf_CreateFromVectorAngle({1, 0, 0}, -menuPitch);
		XrQuaternionf yaw = XrQuaternionf_CreateFromVectorAngle({0, 1, 0}, menuYaw);
		cylinder_layer.pose.orientation = XrQuaternionf_Multiply(pitch, yaw);
		cylinder_layer.pose.position = pos;
		cylinder_layer.radius = 12.0f;
		cylinder_layer.centralAngle = M_PI * 0.5f;
		cylinder_layer.aspectRatio = 1;

		engine->appState.Layers[engine->appState.LayerCount++].Cylinder = cylinder_layer;
	} else {
		assert(false);
	}

	// Compose the layers for this frame.
	const XrCompositionLayerBaseHeader* layers[ovrMaxLayerCount] = {};
	for (int i = 0; i < engine->appState.LayerCount; i++) {
		layers[i] = (const XrCompositionLayerBaseHeader*)&engine->appState.Layers[i];
	}

	XrFrameEndInfo endFrameInfo = {};
	endFrameInfo.type = XR_TYPE_FRAME_END_INFO;
	endFrameInfo.displayTime = frameState.predictedDisplayTime;
	endFrameInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endFrameInfo.layerCount = engine->appState.LayerCount;
	endFrameInfo.layers = layers;

	OXR(xrEndFrame(engine->appState.Session, &endFrameInfo));
	int instances = engine->appState.Renderer.Multiview ? 1 : ovrMaxNumEyes;
	for (int i = 0; i < instances; i++) {
		ovrFramebuffer* frameBuffer = &engine->appState.Renderer.FrameBuffer[instances];
		frameBuffer->TextureSwapChainIndex++;
		frameBuffer->TextureSwapChainIndex %= frameBuffer->TextureSwapChainLength;
	}
}

int VR_GetConfig( VRConfig config ) {
	return vrConfig[config];
}

void VR_SetConfig( VRConfig config, int value) {
	vrConfig[config] = value;
}

void VR_BindFramebuffer(engine_t *engine) {
	if (!initialized) return;
	int fboIndex = vrConfig[VR_CONFIG_CURRENT_FBO];
	ovrFramebuffer* frameBuffer = &engine->appState.Renderer.FrameBuffer[fboIndex];
	unsigned int swapchainIndex = frameBuffer->TextureSwapChainIndex;
	unsigned int glFramebuffer = frameBuffer->FrameBuffers[swapchainIndex];
	GL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, glFramebuffer));
}

ovrMatrix4f VR_GetMatrix( VRMatrix matrix ) {
	ovrMatrix4f output;
	if ((matrix == VR_PROJECTION_MATRIX_LEFT_EYE) || (matrix == VR_PROJECTION_MATRIX_RIGHT_EYE)) {
		XrFovf fov = matrix == VR_PROJECTION_MATRIX_LEFT_EYE ? projections[0].fov : projections[1].fov;
		float near = (float)vrConfig[VR_CONFIG_FOV_SCALE] / 200.0f;
		output = ovrMatrix4f_CreateProjectionFov(fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown, near, 0.0f );
	} else if ((matrix == VR_VIEW_MATRIX_LEFT_EYE) || (matrix == VR_VIEW_MATRIX_RIGHT_EYE)) {
		XrPosef invView = invViewTransform[0];

		// get axis mirroring configuration
		float mx = vrConfig[VR_CONFIG_MIRROR_PITCH] ? -1 : 1;
		float my = vrConfig[VR_CONFIG_MIRROR_YAW] ? -1 : 1;
		float mz = vrConfig[VR_CONFIG_MIRROR_ROLL] ? -1 : 1;

		// ensure there is maximally one axis to mirror rotation
		if (mx + my + mz < 0) {
			mx *= -1.0f;
			my *= -1.0f;
			mz *= -1.0f;
		} else {
			invView = XrPosef_Inverse(invView);
		}

		// create updated quaternion
		if (mx + my + mz < 3 - EPSILON) {
			XrVector3f rotation = XrQuaternionf_ToEulerAngles(invView.orientation);
			XrQuaternionf pitch = XrQuaternionf_CreateFromVectorAngle({1, 0, 0}, mx * ToRadians(rotation.x));
			XrQuaternionf yaw = XrQuaternionf_CreateFromVectorAngle({0, 1, 0}, my * ToRadians(rotation.y));
			XrQuaternionf roll = XrQuaternionf_CreateFromVectorAngle({0, 0, 1}, mz * ToRadians(rotation.z));
			invView.orientation = XrQuaternionf_Multiply(roll, XrQuaternionf_Multiply(pitch, yaw));
		}

		output = ovrMatrix4f_CreateFromQuaternion(&invView.orientation);
		float scale = (float)VR_GetConfig(VR_CONFIG_6DOF_SCALE) * 0.001f;
		if (vrConfig[VR_CONFIG_6DOF_ENABLED]) {
			output.M[0][3] -= hmdposition.x * (vrConfig[VR_CONFIG_MIRROR_AXIS_X] ? -1.0f : 1.0f) * scale;
			output.M[1][3] -= hmdposition.y * (vrConfig[VR_CONFIG_MIRROR_AXIS_Y] ? -1.0f : 1.0f) * scale;
			output.M[2][3] -= hmdposition.z * (vrConfig[VR_CONFIG_MIRROR_AXIS_Z] ? -1.0f : 1.0f) * scale;
		}
		if (matrix == VR_VIEW_MATRIX_RIGHT_EYE) {
			float ipdScale = (float)vrConfig[VR_CONFIG_STEREO_SEPARATION] * 0.1f * scale;
			output.M[0][3] += (invViewTransform[1].position.x - invViewTransform[0].position.x) * ipdScale;
			output.M[1][3] += (invViewTransform[1].position.y - invViewTransform[0].position.y) * ipdScale;
			output.M[2][3] += (invViewTransform[1].position.z - invViewTransform[0].position.z) * ipdScale;
		}
	} else {
		assert(false);
	}
	return output;
}
