local cycleHandlers = {
  first           = { prev = "upscalerPrev",        next = "upscalerNext" },
  toggleShadows   = { prev = "toggleShadows",       next = "toggleShadows" },
  toggleSsr       = { prev = "toggleSsr",           next = "toggleSsr" },
  toggleAo        = { prev = "toggleAo",            next = "toggleAo" },
  toggleSss       = { prev = "toggleSss",           next = "toggleSss" },
  toggleBloom     = { prev = "toggleBloom",       next = "toggleBloom" },
  toggleFog       = { prev = "toggleFog",         next = "toggleFog" },
  toggleTaa       = { prev = "toggleTaa",         next = "toggleTaa" },
  toggleFogMode   = { prev = "toggleFogMode",       next = "toggleFogMode" },
}

function graphicsSettingsKeyDown(event)
  local key = event.parameters["key_identifier"]

  -- escape translates to sdl3 gamepad button east, which is "B" in xbox controller
  if key == rmlui.key_identifier.ESCAPE then
    graphicsClose()
    return
  end

  -- Focus/target can be a child element inside the button (label, slider text,
  -- arrow icon, etc.), so walk up until we reach the owning button.
  local element = event.target_element
  while element and element.tag_name ~= "button" do
    element = element.parent_node
  end

  if element then
    local handler = cycleHandlers[element.id]
    if handler then
      if key == rmlui.key_identifier.LEFT then
        _G[handler.prev]()
      elseif key == rmlui.key_identifier.RIGHT then
        _G[handler.next]()
      end
    end
  end
end
