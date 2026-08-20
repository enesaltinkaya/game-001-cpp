function videoSettingsKeyDown(event)
  local key = event.parameters["key_identifier"]

  -- escape translates to sdl3 gamepad button east, which is "B" in xbox controller
  if key == rmlui.key_identifier.ESCAPE then
    videoClose()
    return
  end
end

function fpsLimitClick(element, event)
  if event.parameters.button == 1 then
    local input = element:QuerySelector("input")
    input:SetAttribute("value", 60);
  end
end
