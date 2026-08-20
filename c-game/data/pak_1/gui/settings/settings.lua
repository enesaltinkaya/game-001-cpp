function settingsKeyDown(event)
  -- escape translates to sdl3 gamepad button east, which is "B" in xbox controller
  if event.parameters["key_identifier"] == rmlui.key_identifier.ESCAPE then
    settingsClose()
  end
end
