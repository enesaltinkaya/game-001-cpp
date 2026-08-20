globalInit = globalInit or 0
if globalInit == 0 then
  globalInit = 1

  function addClass(element, class)
    element.class_name = element.class_name .. " " .. class
  end

  function removeClass(element, class)
    element.class_name = element.class_name:gsub(class, "") -- replace
  end

  function hasClass(element, class)
    return string.find(element.class_name, class)
  end

  function deactivateButtons(element)
    local visible = element.owner_document:QuerySelectorAll("button")
    for k, v in pairs(visible) do
      removeClass(v, "active")
    end
  end

  function focusEvent(event)
    local element = event.target_element
    if element.tag_name ~= "button" then
      return
    end

    if hasClass(element, "disabled") then
      return
    end

    if element.class_name:find("active") then
      return
    end

    luacHoverSound();
    deactivateButtons(element);
    addClass(element, "active")
  end

  function mouseOverEvent(event)
    local element = event.target_element
    if element.tag_name == "button" then
      if hasClass(element, "disabled") then
        return
      end
      element:Focus()
    end
  end

  function radioClick(event)
    local element = event.target_element
    local inputs = element:QuerySelectorAll(".radio")
    for k, input in pairs(inputs) do
      if input.attributes["checked"] == nil then
        input:DispatchEvent('click', { button = event.parameters.button })
        element:Focus()
      end
    end
  end

  function checkBoxClick(event)
    local element = event.current_element
    local inputs = element:QuerySelectorAll("input.checkbox")
    for k, input in pairs(inputs) do
      if input.attributes["checked"] then
        input:RemoveAttribute("checked")
      else
        input:SetAttribute("checked", "1")
      end
      element:Focus()
    end
  end

  function clickEvent(event)
    local element = event.current_element
    local target = event.target_element

    if element.tag_name == "button" then
      if hasClass(element, "disabled") then
        return
      end

      checkBoxClick(event)

      if element.id == "effects" then
        return
      end
      luacClickSound()
    end
  end

  function keyDownEvent(event)
    local element = event.target_element
    local key = event.parameters.key_identifier


    if element.tag_name == "button" then
      if key == rmlui.key_identifier.LEFT or key == rmlui.key_identifier.RIGHT then
        local inputs = element:QuerySelectorAll("input")
        for k, input in pairs(inputs) do
          input:DispatchEvent('keydown', { key_identifier = event.parameters.key_identifier })
        end
      end
    end
  end

  function documentInit(element)
    local buttons = element:QuerySelectorAll("button")
    for k, button in pairs(buttons) do
      button:AddEventListener("keydown", keyDownEvent, "")
      button:AddEventListener("click", clickEvent, "")
      button:AddEventListener("focus", focusEvent, "")
      button:AddEventListener("mouseover", mouseOverEvent, "")
    end
  end
end
