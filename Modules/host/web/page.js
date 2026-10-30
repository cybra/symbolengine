// The page of a plugin's parameters, shown where it has no editor of its own: a row for each, with a slider, a choice
// of its steps or a switch, and the plugin's own text for its value. A move here reaches the plugin as its own editor's
// would, and values changed elsewhere are followed.

const list = document.getElementById("parameters");
const none = document.getElementById("none");
const rows = new Map();

function element(tag, className, text) {
    const made = document.createElement(tag);
    if (className)
        made.className = className;
    if (text !== undefined)
        made.textContent = text;
    return made;
}

// The control for a parameter: a switch for one of two steps, a choice for a few, a slider otherwise.
async function controlFor(parameter) {
    if (parameter.steps === 1) {
        const toggle = element("input");
        toggle.type = "checkbox";
        toggle.checked = parameter.value >= 0.5;
        return {
            control: toggle,
            valueOf: () => (toggle.checked ? 1 : 0),
            show: value => (toggle.checked = value >= 0.5),
        };
    }

    if (parameter.steps > 1 && parameter.steps <= 128) {
        const choice = element("select");
        const texts = await symbolengine.call("stepTexts", parameter.id);
        texts.forEach((text, step) => {
            const option = element("option", "", text);
            option.value = String(step);
            choice.append(option);
        });
        const show = value => (choice.value = String(Math.round(value * parameter.steps)));
        show(parameter.value);
        return { control: choice, valueOf: () => Number(choice.value) / parameter.steps, show };
    }

    const slider = element("input");
    slider.type = "range";
    slider.min = "0";
    slider.max = "1";
    slider.step = "any";
    slider.value = String(parameter.value);
    return { control: slider, valueOf: () => Number(slider.value), show: value => (slider.value = String(value)) };
}

// A slider's gesture runs from its press to its release, or from the first change made from the keyboard to its end;
// a switch or a choice is a gesture of one change.
function wire(row, parameter) {
    const id = parameter.id;
    const set = async () => (row.text.textContent = await symbolengine.call("set", id, row.valueOf()));
    const begin = () => {
        if (!row.held) {
            row.held = true;
            symbolengine.call("begin", id);
        }
    };
    const end = () => {
        if (row.held) {
            row.held = false;
            symbolengine.call("end", id);
        }
    };

    if (row.control.type === "range") {
        row.control.addEventListener("pointerdown", begin);
        row.control.addEventListener("input", () => {
            begin();
            set();
        });
        row.control.addEventListener("change", end);
        return;
    }

    row.control.addEventListener("change", () => {
        begin();
        set();
        end();
    });
}

async function show() {
    const parameters = await symbolengine.call("parameters");
    const made = [];

    for (const parameter of parameters) {
        const line = element("div", "row");
        line.dataset.parameter = String(parameter.id);
        const row = { ...(await controlFor(parameter)), text: element("span", "text", parameter.text), held: false };
        row.control.disabled = parameter.readOnly;
        wire(row, parameter);
        line.append(element("span", "name", parameter.name), row.control, row.text);
        made.push([parameter.id, row, line]);
    }

    rows.clear();
    list.replaceChildren(...made.map(([, , line]) => line));
    made.forEach(([id, row]) => rows.set(id, row));
    none.hidden = parameters.length > 0;
}

symbolengine.on("values", changed => {
    for (const { id, value, text } of changed) {
        const row = rows.get(id);
        if (!row)
            continue;
        row.text.textContent = text;
        if (!row.held)
            row.show(value);
    }
});

symbolengine.on("parameters", show);
show();
