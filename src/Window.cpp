#include <map>
#include <queue>
#include <thread>

#if defined ARCH_MAC
	// For CGAssociateMouseAndMouseCursorPosition
	#include <ApplicationServices/ApplicationServices.h>
#endif

#include <stb_image_write.h>
#include <osdialog.h>

#include <Window.hpp>
#include <asset.hpp>
#include <widget/Widget.hpp>
#include <app/Scene.hpp>
#include <keyboard.hpp>
#include <gamepad.hpp>
#include <context.hpp>
#include <patch.hpp>
#include <settings.hpp>
#include <plugin.hpp> // used in Window::screenshot
#include <system.hpp> // used in Window::screenshot


namespace rack {


static const math::Vec minWindowSize = math::Vec(640, 480);


void Font::loadFile(const std::string& filename, NVGcontext* vg) {
	this->vg = vg;
	handle = nvgCreateFont(vg, filename.c_str(), filename.c_str());
	if (handle < 0)
		throw Exception("Failed to load font %s", filename.c_str());
	INFO("Loaded font %s", filename.c_str());
}


Font::~Font() {
	// There is no NanoVG deleteFont() function yet, so do nothing
}


std::shared_ptr<Font> Font::load(const std::string& filename) {
	return APP->window->loadFont(filename);
}


void Image::loadFile(const std::string& filename, NVGcontext* vg) {
	this->vg = vg;
	handle = nvgCreateImage(vg, filename.c_str(), NVG_IMAGE_REPEATX | NVG_IMAGE_REPEATY);
	if (handle <= 0)
		throw Exception("Failed to load image %s", filename.c_str());
	INFO("Loaded image %s", filename.c_str());
}


Image::~Image() {
	// TODO What if handle is invalid?
	if (handle >= 0)
		nvgDeleteImage(vg, handle);
}


std::shared_ptr<Image> Image::load(const std::string& filename) {
	return APP->window->loadImage(filename);
}


struct Window::Internal {
	std::string lastWindowTitle;

	int lastWindowX = 0;
	int lastWindowY = 0;
	int lastWindowWidth = 0;
	int lastWindowHeight = 0;

	int frame = 0;
	bool ignoreNextMouseDelta = false;
	int frameSwapInterval = -1;
	double monitorRefreshRate = 0.0;
	double frameTime = 0.0;
	double lastFrameDuration = 0.0;

	math::Vec lastMousePos;

	std::map<std::string, std::shared_ptr<Font>> fontCache;
	std::map<std::string, std::shared_ptr<Image>> imageCache;

	bool fbDirtyOnSubpixelChange = true;
};


static void windowPosCallback(GLFWwindow* win, int x, int y) {
	if (glfwGetWindowAttrib(win, GLFW_MAXIMIZED))
		return;
	if (glfwGetWindowMonitor(win))
		return;
	settings::windowPos = math::Vec(x, y);
	// DEBUG("windowPosCallback %d %d", x, y);
}


static void windowSizeCallback(GLFWwindow* win, int width, int height) {
	if (glfwGetWindowAttrib(win, GLFW_MAXIMIZED))
		return;
	if (glfwGetWindowMonitor(win))
		return;
	settings::windowSize = math::Vec(width, height);
	// DEBUG("windowSizeCallback %d %d", width, height);
}


static void windowMaximizeCallback(GLFWwindow* win, int maximized) {
	settings::windowMaximized = maximized;
	// DEBUG("windowMaximizeCallback %d", maximized);
}


static void mouseButtonCallback(GLFWwindow* win, int button, int action, int mods) {
	contextSet((Context*) glfwGetWindowUserPointer(win));
#if defined ARCH_MAC
	// Remap Ctrl-left click to right click on Mac
	if (button == GLFW_MOUSE_BUTTON_LEFT && (mods & RACK_MOD_MASK) == GLFW_MOD_CONTROL) {
		button = GLFW_MOUSE_BUTTON_RIGHT;
		mods &= ~GLFW_MOD_CONTROL;
	}
	// Remap Ctrl-shift-left click to middle click on Mac
	if (button == GLFW_MOUSE_BUTTON_LEFT && (mods & RACK_MOD_MASK) == (GLFW_MOD_CONTROL | GLFW_MOD_SHIFT)) {
		button = GLFW_MOUSE_BUTTON_MIDDLE;
		mods &= ~(GLFW_MOD_CONTROL | GLFW_MOD_SHIFT);
	}
#endif

	APP->event->handleButton(APP->window->internal->lastMousePos, button, action, mods);
}


static void cursorPosCallback(GLFWwindow* win, double xpos, double ypos) {
	contextSet((Context*) glfwGetWindowUserPointer(win));
	math::Vec mousePos = math::Vec(xpos, ypos).div(APP->window->pixelRatio / APP->window->windowRatio).round();
	math::Vec mouseDelta = mousePos.minus(APP->window->internal->lastMousePos);

	// Workaround for GLFW warping mouse to a different position when the cursor is locked or unlocked.
	if (APP->window->internal->ignoreNextMouseDelta) {
		APP->window->internal->ignoreNextMouseDelta = false;
		mouseDelta = math::Vec();
	}

	int cursorMode = glfwGetInputMode(win, GLFW_CURSOR);
	(void) cursorMode;

#if defined ARCH_MAC
	// Workaround for Mac. We can't use GLFW_CURSOR_DISABLED because it's buggy, so implement it on our own.
	// This is not an ideal implementation. For example, if the user drags off the screen, the new mouse position will be clamped.
	if (cursorMode == GLFW_CURSOR_HIDDEN) {
		// CGSetLocalEventsSuppressionInterval(0.0);
		glfwSetCursorPos(win, APP->window->internal->lastMousePos.x, APP->window->internal->lastMousePos.y);
		CGAssociateMouseAndMouseCursorPosition(true);
		mousePos = APP->window->internal->lastMousePos;
	}
	// Because sometimes the cursor turns into an arrow when its position is on the boundary of the window
	glfwSetCursor(win, NULL);
#endif

	APP->window->internal->lastMousePos = mousePos;

	APP->event->handleHover(mousePos, mouseDelta);

	// Keyboard/mouse MIDI driver
	int width, height;
	glfwGetWindowSize(win, &width, &height);
	math::Vec scaledPos(xpos / width, ypos / height);
	keyboard::mouseMove(scaledPos);
}


static void cursorEnterCallback(GLFWwindow* win, int entered) {
	contextSet((Context*) glfwGetWindowUserPointer(win));
	if (!entered) {
		APP->event->handleLeave();
	}
}


static void scrollCallback(GLFWwindow* win, double x, double y) {
	contextSet((Context*) glfwGetWindowUserPointer(win));
	math::Vec scrollDelta = math::Vec(x, y);
#if defined ARCH_MAC
	scrollDelta = scrollDelta.mult(10.0);
#else
	scrollDelta = scrollDelta.mult(50.0);
#endif

	APP->event->handleScroll(APP->window->internal->lastMousePos, scrollDelta);
}


static void charCallback(GLFWwindow* win, unsigned int codepoint) {
	contextSet((Context*) glfwGetWindowUserPointer(win));
	if (APP->event->handleText(APP->window->internal->lastMousePos, codepoint))
		return;
}


static void keyCallback(GLFWwindow* win, int key, int scancode, int action, int mods) {
	contextSet((Context*) glfwGetWindowUserPointer(win));
	if (APP->event->handleKey(APP->window->internal->lastMousePos, key, scancode, action, mods))
		return;

	// Keyboard/mouse MIDI driver
	if (action == GLFW_PRESS && (mods & RACK_MOD_MASK) == 0) {
		keyboard::press(key);
	}
	if (action == GLFW_RELEASE) {
		keyboard::release(key);
	}
}


static void dropCallback(GLFWwindow* win, int count, const char** paths) {
	contextSet((Context*) glfwGetWindowUserPointer(win));
	std::vector<std::string> pathsVec;
	for (int i = 0; i < count; i++) {
		pathsVec.push_back(paths[i]);
	}
	APP->event->handleDrop(APP->window->internal->lastMousePos, pathsVec);
}


static void errorCallback(int error, const char* description) {
	WARN("GLFW error %d: %s", error, description);
}


Window::Window() {
	internal = new Internal;
	int err;

	// Set window hints
#if defined NANOVG_GL2
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#elif defined NANOVG_GL3
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif
	glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

#if defined ARCH_MAC
	glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif

	// Create window
	win = glfwCreateWindow(1024, 768, "", NULL, NULL);
	if (!win) {
		osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, "Could not open GLFW window. Does your graphics card support OpenGL 2.0 or greater? If so, make sure you have the latest graphics drivers installed.");
		exit(1);
	}

	float contentScale;
	glfwGetWindowContentScale(win, &contentScale, NULL);
	INFO("Window content scale: %f", contentScale);

	glfwSetWindowSizeLimits(win, minWindowSize.x, minWindowSize.y, GLFW_DONT_CARE, GLFW_DONT_CARE);
	glfwSetWindowSize(win, settings::windowSize.x, settings::windowSize.y);
	if (settings::windowPos.isFinite()) {
		glfwSetWindowPos(win, settings::windowPos.x, settings::windowPos.y);
	}
	if (settings::windowMaximized) {
		glfwMaximizeWindow(win);
	}
	glfwShowWindow(win);

	glfwSetWindowUserPointer(win, contextGet());
	glfwSetInputMode(win, GLFW_LOCK_KEY_MODS, 1);

	glfwMakeContextCurrent(win);
	glfwSwapInterval(1);
	const GLFWvidmode* monitorMode = glfwGetVideoMode(glfwGetPrimaryMonitor());
	internal->monitorRefreshRate = monitorMode->refreshRate;

	// Set window callbacks
	glfwSetWindowPosCallback(win, windowPosCallback);
	glfwSetWindowSizeCallback(win, windowSizeCallback);
	glfwSetWindowMaximizeCallback(win, windowMaximizeCallback);
	glfwSetMouseButtonCallback(win, mouseButtonCallback);
	// Call this ourselves, but on every frame instead of only when the mouse moves
	// glfwSetCursorPosCallback(win, cursorPosCallback);
	glfwSetCursorEnterCallback(win, cursorEnterCallback);
	glfwSetScrollCallback(win, scrollCallback);
	glfwSetCharCallback(win, charCallback);
	glfwSetKeyCallback(win, keyCallback);
	glfwSetDropCallback(win, dropCallback);

	// Set up GLEW
	glewExperimental = GL_TRUE;
	err = glewInit();
	if (err != GLEW_OK) {
		osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, "Could not initialize GLEW. Does your graphics card support OpenGL 2.0 or greater? If so, make sure you have the latest graphics drivers installed.");
		exit(1);
	}

	const GLubyte* vendor = glGetString(GL_VENDOR);
	const GLubyte* renderer = glGetString(GL_RENDERER);
	const GLubyte* version = glGetString(GL_VERSION);
	INFO("Renderer: %s %s", vendor, renderer);
	INFO("OpenGL: %s", version);

	// GLEW generates GL error because it calls glGetString(GL_EXTENSIONS), we'll consume it here.
	glGetError();

	// Set up NanoVG
	int nvgFlags = NVG_ANTIALIAS;
#if defined NANOVG_GL2
	vg = nvgCreateGL2(nvgFlags);
	// TODO Instead of creating a whole other NanoVG context, create one that uses the same fonts and framebuffers as `fb`.
	fbVg = nvgCreateGL2(nvgFlags);
#elif defined NANOVG_GL3
	vg = nvgCreateGL3(nvgFlags);
#elif defined NANOVG_GLES2
	vg = nvgCreateGLES2(nvgFlags);
#endif
	if (!vg) {
		osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, "Could not initialize NanoVG. Does your graphics card support OpenGL 2.0 or greater? If so, make sure you have the latest graphics drivers installed.");
		exit(1);
	}

	// fbVg = nvgClone(vg);

	// Load default Blendish font
	uiFont = loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
	bndSetFont(uiFont->handle);

	if (APP->scene) {
		widget::Widget::ContextCreateEvent e;
		APP->scene->onContextCreate(e);
	}
}


Window::~Window() {
	if (APP->scene) {
		widget::Widget::ContextDestroyEvent e;
		APP->scene->onContextDestroy(e);
	}

	// nvgDeleteClone(fbVg);

#if defined NANOVG_GL2
	nvgDeleteGL2(vg);
	nvgDeleteGL2(fbVg);
#elif defined NANOVG_GL3
	nvgDeleteGL3(vg);
#elif defined NANOVG_GLES2
	nvgDeleteGLES2(vg);
#endif

	glfwDestroyWindow(win);
	delete internal;
}


math::Vec Window::getSize() {
	int width, height;
	glfwGetWindowSize(win, &width, &height);
	return math::Vec(width, height);
}


void Window::setSize(math::Vec size) {
	size = size.max(minWindowSize);
	glfwSetWindowSize(win, size.x, size.y);
}


void Window::run() {
	internal->frame = 0;
	while (!glfwWindowShouldClose(win)) {
		step();
	}
}


void Window::step() {
	double frameTime = system::getTime();
	double lastFrameTime = internal->frameTime;
	internal->frameTime = frameTime;
	internal->lastFrameDuration = frameTime - lastFrameTime;
	// DEBUG("%.2lf Hz", 1.0 / internal->lastFrameDuration);
	double t1 = 0.0, t2 = 0.0, t3 = 0.0, t4 = 0.0, t5 = 0.0;

	// Make event handlers and step() have a clean NanoVG context
	nvgReset(vg);

	bndSetFont(uiFont->handle);

	// Poll events
	// Save and restore context because event handler set their own context based on which window they originate from.
	Context* context = contextGet();
	glfwPollEvents();
	contextSet(context);

	// In case glfwPollEvents() sets another OpenGL context
	glfwMakeContextCurrent(win);
	if (settings::frameSwapInterval != internal->frameSwapInterval) {
		glfwSwapInterval(settings::frameSwapInterval);
		internal->frameSwapInterval = settings::frameSwapInterval;
	}

	// Call cursorPosCallback every frame, not just when the mouse moves
	{
		double xpos, ypos;
		glfwGetCursorPos(win, &xpos, &ypos);
		cursorPosCallback(win, xpos, ypos);
	}
	gamepad::step();

	// Set window title
	std::string windowTitle = APP_NAME + " " + APP_VARIANT + " " + APP_VERSION;
	if (APP->patch->path != "") {
		windowTitle += " - ";
		if (!APP->history->isSaved())
			windowTitle += "*";
		windowTitle += system::getFilename(APP->patch->path);
	}
	if (windowTitle != internal->lastWindowTitle) {
		glfwSetWindowTitle(win, windowTitle.c_str());
		internal->lastWindowTitle = windowTitle;
	}

	// Get desired scaling
	float newPixelRatio;
	glfwGetWindowContentScale(win, &newPixelRatio, NULL);
	newPixelRatio = std::floor(newPixelRatio + 0.5);
	if (newPixelRatio != pixelRatio) {
		APP->event->handleDirty();
		pixelRatio = newPixelRatio;
	}

	// Get framebuffer/window ratio
	int fbWidth, fbHeight;
	glfwGetFramebufferSize(win, &fbWidth, &fbHeight);
	int winWidth, winHeight;
	glfwGetWindowSize(win, &winWidth, &winHeight);
	windowRatio = (float)fbWidth / winWidth;
	t1 = system::getTime();

	if (APP->scene) {
		// DEBUG("%f %f %d %d", pixelRatio, windowRatio, fbWidth, winWidth);
		// Resize scene
		APP->scene->box.size = math::Vec(fbWidth, fbHeight).div(pixelRatio);

		// Step scene
		APP->scene->step();
		t2 = system::getTime();

		// Render scene
		bool visible = glfwGetWindowAttrib(win, GLFW_VISIBLE) && !glfwGetWindowAttrib(win, GLFW_ICONIFIED);
		if (visible) {
			// Update and render
			nvgBeginFrame(vg, fbWidth, fbHeight, pixelRatio);
			nvgScale(vg, pixelRatio, pixelRatio);

			// Draw scene
			widget::Widget::DrawArgs args;
			args.vg = vg;
			args.clipBox = APP->scene->box.zeroPos();
			APP->scene->draw(args);
			t3 = system::getTime();

			glViewport(0, 0, fbWidth, fbHeight);
			glClearColor(0.0, 0.0, 0.0, 1.0);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
			nvgEndFrame(vg);
			t4 = system::getTime();
		}
	}

	glfwSwapBuffers(win);
	t5 = system::getTime();

	// DEBUG("pre-step %6.1f step %6.1f draw %6.1f nvgEndFrame %6.1f glfwSwapBuffers %6.1f total %6.1f",
	// 	(t1 - frameTime) * 1e3f,
	// 	(t2 - t1) * 1e3f,
	// 	(t3 - t2) * 1e3f,
	// 	(t4 - t2) * 1e3f,
	// 	(t5 - t4) * 1e3f,
	// 	(t5 - frameTime) * 1e3f
	// );
	internal->frame++;
}


static void flipBitmap(uint8_t* pixels, int width, int height, int depth) {
	for (int y = 0; y < height / 2; y++) {
		int flipY = height - y - 1;
		uint8_t tmp[width * depth];
		std::memcpy(tmp, &pixels[y * width * depth], width * depth);
		std::memcpy(&pixels[y * width * depth], &pixels[flipY * width * depth], width * depth);
		std::memcpy(&pixels[flipY * width * depth], tmp, width * depth);
	}
}


void Window::screenshot(const std::string& screenshotPath) {
	// Get window framebuffer size
	int width, height;
	glfwGetFramebufferSize(APP->window->win, &width, &height);

	// Allocate pixel color buffer
	uint8_t* pixels = new uint8_t[height * width * 4];

	// glReadPixels defaults to GL_BACK, but the back-buffer is unstable, so use the front buffer (what the user sees)
	glReadBuffer(GL_FRONT);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

	// Write pixels to PNG
	flipBitmap(pixels, width, height, 4);
	stbi_write_png(screenshotPath.c_str(), width, height, 4, pixels, width * 4);

	delete[] pixels;
}


void Window::screenshotModules(const std::string& screenshotsDir, float zoom) {
	// Iterate plugins and create directories
	system::createDirs(screenshotsDir);
	for (plugin::Plugin* p : plugin::plugins) {
		std::string dir = system::join(screenshotsDir, p->slug);
		system::createDir(dir);
		for (plugin::Model* model : p->models) {
			std::string filename = system::join(dir, model->slug + ".png");

			// Skip model if screenshot already exists
			if (system::isFile(filename))
				continue;

			INFO("Screenshotting %s %s to %s", p->slug.c_str(), model->slug.c_str(), filename.c_str());

			// Create widgets
			widget::FramebufferWidget* fbw = new widget::FramebufferWidget;
			fbw->oversample = 2;

			app::ModuleWidget* mw = model->createModuleWidget(NULL);
			fbw->addChild(mw);
			fbw->box.size = mw->box.size;

			// Draw to framebuffer
			fbw->render(math::Vec(zoom, zoom), math::Vec(0, 0));

			// Read pixels
			nvgluBindFramebuffer(fbw->getFramebuffer());
			int width, height;
			nvgImageSize(vg, fbw->getImageHandle(), &width, &height);
			uint8_t* pixels = new uint8_t[height * width * 4];
			glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

			// Write pixels to PNG
			flipBitmap(pixels, width, height, 4);
			stbi_write_png(filename.c_str(), width, height, 4, pixels, width * 4);

			// Cleanup
			delete[] pixels;
			nvgluBindFramebuffer(NULL);
			delete fbw;
		}
	}
}


void Window::close() {
	glfwSetWindowShouldClose(win, GLFW_TRUE);
}


void Window::cursorLock() {
	if (!settings::allowCursorLock)
		return;

#if defined ARCH_MAC
	glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
#else
	glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
#endif
	internal->ignoreNextMouseDelta = true;
}


void Window::cursorUnlock() {
	if (!settings::allowCursorLock)
		return;

	glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
	internal->ignoreNextMouseDelta = true;
}


bool Window::isCursorLocked() {
	return glfwGetInputMode(win, GLFW_CURSOR) != GLFW_CURSOR_NORMAL;
}


int Window::getMods() {
	int mods = 0;
	if (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS)
		mods |= GLFW_MOD_SHIFT;
	if (glfwGetKey(win, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS)
		mods |= GLFW_MOD_CONTROL;
	if (glfwGetKey(win, GLFW_KEY_LEFT_ALT) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS)
		mods |= GLFW_MOD_ALT;
	if (glfwGetKey(win, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS)
		mods |= GLFW_MOD_SUPER;
	return mods;
}


void Window::setFullScreen(bool fullScreen) {
	if (!fullScreen) {
		glfwSetWindowMonitor(win, NULL, internal->lastWindowX, internal->lastWindowY, internal->lastWindowWidth, internal->lastWindowHeight, GLFW_DONT_CARE);
	}
	else {
		glfwGetWindowPos(win, &internal->lastWindowX, &internal->lastWindowY);
		glfwGetWindowSize(win, &internal->lastWindowWidth, &internal->lastWindowHeight);
		GLFWmonitor* monitor = glfwGetPrimaryMonitor();
		const GLFWvidmode* mode = glfwGetVideoMode(monitor);
		glfwSetWindowMonitor(win, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
	}
}


bool Window::isFullScreen() {
	GLFWmonitor* monitor = glfwGetWindowMonitor(win);
	return monitor != NULL;
}


double Window::getMonitorRefreshRate() {
	return internal->monitorRefreshRate;
}


double Window::getFrameTime() {
	return internal->frameTime;
}


double Window::getLastFrameDuration() {
	return internal->lastFrameDuration;
}


double Window::getFrameTimeOverdue() {
	double desiredFrameDuration = internal->frameSwapInterval / internal->monitorRefreshRate;
	double frameDuration = system::getTime() - internal->frameTime;
	return frameDuration - desiredFrameDuration;
}


std::shared_ptr<Font> Window::loadFont(const std::string& filename) {
	const auto& pair = internal->fontCache.find(filename);
	if (pair != internal->fontCache.end())
		return pair->second;

	// Load font
	std::shared_ptr<Font> font;
	try {
		font = std::make_shared<Font>();
		font->loadFile(filename, vg);
	}
	catch (Exception& e) {
		WARN("%s", e.what());
		font = NULL;
	}
	internal->fontCache[filename] = font;
	return font;
}


std::shared_ptr<Image> Window::loadImage(const std::string& filename) {
	const auto& pair = internal->imageCache.find(filename);
	if (pair != internal->imageCache.end())
		return pair->second;

	// Load image
	std::shared_ptr<Image> image;
	try {
		image = std::make_shared<Image>();
		image->loadFile(filename, vg);
	}
	catch (Exception& e) {
		WARN("%s", e.what());
		image = NULL;
	}
	internal->imageCache[filename] = image;
	return image;
}


bool& Window::fbDirtyOnSubpixelChange() {
	return internal->fbDirtyOnSubpixelChange;
}


void windowInit() {
	int err;

	// Set up GLFW
#if defined ARCH_MAC
	glfwInitHint(GLFW_COCOA_CHDIR_RESOURCES, GLFW_TRUE);
	glfwInitHint(GLFW_COCOA_MENUBAR, GLFW_TRUE);
#endif

	glfwSetErrorCallback(errorCallback);
	err = glfwInit();
	if (err != GLFW_TRUE) {
		osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, "Could not initialize GLFW.");
		exit(1);
	}
}


void windowDestroy() {
	glfwTerminate();
}


} // namespace rack
