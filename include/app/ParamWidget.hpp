#pragma once
#include <app/common.hpp>
#include <widget/OpaqueWidget.hpp>
#include <ui/Tooltip.hpp>
#include <ui/Menu.hpp>
#include <engine/ParamQuantity.hpp>


namespace rack {
namespace app {


/** Manages an engine::Param on a ModuleWidget. */
struct ParamWidget : widget::OpaqueWidget {
	engine::Module* module = NULL;
	int paramId = 0;

	ui::Tooltip* tooltip = NULL;
	/** For triggering the Change event. `*/
	float lastValue = NAN;

	/** Configures ParamQuantity properties based on the type of ParamWidget.
	This seems a bit hacky, but it's easier than requiring plugin developers to set `ParamQuantity::randomizeEnabled`, etc.
	*/
	virtual void initParamQuantity() {}
	engine::ParamQuantity* getParamQuantity();
	void createTooltip();
	void destroyTooltip();

	void step() override;
	void draw(const DrawArgs& args) override;

	void onButton(const event::Button& e) override;
	void onDoubleClick(const event::DoubleClick& e) override;
	void onEnter(const event::Enter& e) override;
	void onLeave(const event::Leave& e) override;

	void createContextMenu();
	virtual void appendContextMenu(ui::Menu* menu) {}
	void resetAction();
};


} // namespace app
} // namespace rack
