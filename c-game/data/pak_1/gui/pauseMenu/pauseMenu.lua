function pauseKeyDown(event)
    local key = event.parameters.key_identifier
    if key == rmlui.key_identifier.ESCAPE then
        pauseReturnToGame()
    end
end
