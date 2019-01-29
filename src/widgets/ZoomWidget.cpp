#include "widgets/ZoomWidget.hpp"


namespace rack {


math::Vec ZoomWidget::getRelativeOffset(math::Vec v, Widget *relative) {
	return Widget::getRelativeOffset(v.mult(zoom), relative);
}

math::Rect ZoomWidget::getViewport(math::Rect r) {
	r.pos = r.pos.mult(zoom);
	r.size = r.size.mult(zoom);
	r = Widget::getViewport(r);
	r.pos = r.pos.div(zoom);
	r.size = r.size.div(zoom);
	return r;
}

void ZoomWidget::setZoom(float zoom) {
	if (zoom == this->zoom)
		return;
	this->zoom = zoom;

	event::Context eZoomContext;
	event::Zoom eZoom;
	eZoom.context = &eZoomContext;
	Widget::onZoom(eZoom);
}

void ZoomWidget::draw(const DrawContext &ctx) {
	DrawContext zoomCtx = ctx;
	zoomCtx.clipBox.pos = zoomCtx.clipBox.pos.div(zoom);
	zoomCtx.clipBox.size = zoomCtx.clipBox.size.div(zoom);
	// No need to save the state because that is done in the parent
	nvgScale(ctx.vg, zoom, zoom);
	Widget::draw(zoomCtx);
}


} // namespace rack