(() => {

    if ("undefined" == typeof SiebelApp) {
        alert("It works only in Siebel OUI session!");
        return;
    }

    const Id = "XapuksAboutView2";

    // read options localStorage
    let options = {};
    if (localStorage.getItem(Id)) {
        options = JSON.parse(localStorage.getItem(Id));
    } else {
        resetOptions();
    }

    let $d = $(`.ui-dialog.${Id}`);

    // event handling
    const handlers = {
        "None": (e) => {
            return true;
        },
        "Expand / Special": (e) => {
            id = $(e.target).attr("data-ul");
            if (id) {
                $d.find(`ul.ul_show:not(#${id}):not(:has(#${id})):not(.keep_open)`).removeClass("ul_show").addClass("ul_hide");
                $d.find("#" + id).toggleClass(['ul_show', 'ul_hide']);
                e.stopPropagation();
                return false;
            } else {
                e.stopPropagation();
                return true;
            }
        },
        "Expand active context": (e) => {
            const a = SiebelApp.S_App.GetActiveView().GetActiveApplet();
            if (a) {
                $d.find(`ul#${a.GetFullId()}, ul#${a.GetFullId()}_controls, ul#${a.GetActiveControl()?.GetInputName()}`).removeClass("ul_hide").addClass("ul_show");
            }
            e?.stopPropagation();
            return false;
        },
        "Copy active applet": (e) => {
            const a = SiebelApp.S_App.GetActiveView().GetActiveApplet();
            if (a) {
                handlers["Copy value"]({
                    target: $d.find(`a[data-ul=${a.GetFullId()}]`)[0]
                });
            }
        },
        "Collapse item": (e) => {
            $d.find(`ul.ul_show:not(:has(.ul_show:not(.keep_open))):not(.keep_open)`).removeClass("ul_show").addClass("ul_hide");
            e.stopPropagation();
            return false;
        },
        "Collapse all": (e) => {
            $d.find(`ul.ul_show:not(.keep_open)`).removeClass("ul_show").addClass("ul_hide");
            e.stopPropagation();
            return false;
        },
        "Close dialog": (e) => {
            $d.dialog("close");
            e.stopPropagation();
            return false;
        },
        "Options": (e) => {
            rOptions();
            e.stopPropagation();
            return false;
        },
        "Copy value and close": (e) => {
            handlers["Copy value"](e);
            handlers["Close dialog"](e);
            e.stopPropagation?.call(this);
            return false;
        },
        "Copy value": (e) => {
            const scope = e.target;
            // replacing link with intput and select the value
            const val = $(scope).text();
            $(scope).hide().after("<input id='" + Id + "i'>");
            $d.find("#" + Id + "i").val(val).select();
            // attempt to copy value
            if (document.execCommand("copy", false, null)) {
                // if copied, display a message for a second
                $d.find("#" + Id + "i").attr("disabled", "disabled").css("color", "red").val("Copied!");
                setTimeout(() => {
                    $d.find("#" + Id + "i").remove();
                    $(scope).show();
                }, 700);
            } else {
                // if failed to copy, leave input until blur, so it can be copied manually
                $d.find("#" + Id + "i").blur(() => {
                    $(this).remove();
                    $d.find("a").show();
                });
            }
            e.stopPropagation?.call(this);
            return false;
        },
        "Invoke applet method": (e) => {
            var $target = $(e.target);
            var applet = SiebelApp.S_App.GetActiveView().GetAppletMap()[$target.attr("data-applet")];
            const method = $target.text();
            applet.InvokeMethod(method);
            e.stopPropagation?.call(this);
            return false;
        },
        "Invoke control method": (e) => {
            var $target = $(e.target);
            var applet = SiebelApp.S_App.GetActiveView().GetAppletMap()[$target.attr("data-applet")];
            var control = applet.GetControls()[$target.attr("data-control")];
            const method = $target.text();
            SiebelApp.S_App.uiStatus.Busy();
            try {
                applet.GetPModel().OnControlEvent(SiebelApp.Constants.get("PHYEVENT_INVOKE_CONTROL"), control.GetMethodName(), control.GetMethodPropSet(), {
                    async: true,
                    cb: () => SiebelApp.S_App.uiStatus.Free()
                });
            } catch (e) {
                console.error(e.toString());
                SiebelApp.S_App.uiStatus.Free();
            }
            e.stopPropagation?.call(this);
            return false;
        },
        "Focus": (e) => {
            var $target = $(e.target);
            const $el = $($target.attr("data-focus"));
            $d.dialog('close');
            $el.focus();
            e.stopPropagation?.call(this);
            return false;
        },
        "Expand related": (e) => {
            var $target = $(e.target);
            var sel = $target.attr("data-selector");
            $d.find(`ul.ul_show:not(.keep_open)`).removeClass("ul_show").addClass("ul_hide");
            $d.find(sel).toggleClass(['ul_show', 'ul_hide']);
            e.stopPropagation?.call(this);
            return false;
        }

    };

    // handle double click
    if ($d.length) {
        var o = options["bmk_dbl"];
        handlers[o]();
        return;
    }

    // render the dialog
    let guid = 0;
    const css = [`<style>`, ...[
        `ul {margin-left:20px}`,
        `a.x_active {background-color:lightgreen; color:darkblue; font-weight:bold}`,
        `a.x_hidden {background-color:#f0f0f0; color:#888; font-style:italic}`,
        `a {color: blue}`,
        `select {display:block; margin-bottom:15px}`,
        `.options {background-color:lightgray; padding:15px; margin:10px}`,
        `.ul_hide {display:none}`,
        `.ul_show {border-bottom: 1px solid; border-top: 1px solid; margin: 5px; padding: 5px; border-color: lightgray;}`,
        options["focus_feature"] == "true" ? `ul:has(.ul_show:not(.keep_open)) :is(label,a) {color:darkgray!important}` : ``,
        options["focus_feature"] == "true" ? `ul.ul_show:not(:has(.ul_show:not(.keep_open))) :is(label,a) {color:black!important}` : ``,
        `a[data-ul] {font-weight:bold;color:green}`,
        `a[data-ul]:before {content:"> "; opacity:0.5; font-style:normal}`,
        `a[data-handler]:before, a[data-focus]:before {content:"<["; opacity:0.5; font-style:normal}`,
        `a[data-handler]:after, a[data-focus]:after {content:"]>"; opacity:0.5; font-style:normal}`,
        `label {font-size:1rem; margin:0px; font-weight:bold;}`,
        `table {display:block; overflow-x:auto; whitespace: nowrap}`,
        `td {border:solid 1px}`,
        `.options select {width:250px}`,
    ].map((i) => i ? `.${Id} ${i}` : `color:blue`), `</style>`].join("\n");

    $d = $(`<div class="container" title="About View 2.03">${rApplication()}${css}</div>`).dialog({
        dragStop: () => $d.dialog({ height: 'auto' }),
        classes: { "ui-dialog": Id },
        modal: true,
        width: options["width"],
        close: () => $d.dialog('destroy').remove(),
        buttons: [
            {
                text: 'Help',
                click: () => window.open('http://xapuk.com/index.php?topic=145', '_blank')
            }, {
                text: 'Settings',
                click: rOptions,
            }, {
                text: 'Reset Settings',
                click: resetOptions
            }, {
                text: 'Close (esc)',
                click: () => $d.dialog('close')
            }
        ]
    });

    function dispatchEvent(e, cb) {
        var $target = $(e.target);
        if (cb.name?.indexOf("Special") > 0) {
            if ($target.attr("data-handler") == "applet method") {
                return handlers["Invoke applet method"](e);
            } else if ($target.attr("data-handler") == "control method") {
                return handlers["Invoke control method"](e);
            } else if ($target.attr("data-selector")) {
                return handlers["Expand related"](e);
            }
        }
        if ($target.attr("data-focus")) {
            return handlers["Focus"](e);
        }
        return cb(e);
    }

    $d.contextmenu(handlers[options["ws_right"]]);
    $d.click(handlers[options["ws_click"]]);
    $d.find("a").off("click").click((e) => dispatchEvent(e, handlers[options["link_click"]]));
    $d.find("a").off("contextmenu").contextmenu((e) => dispatchEvent(e, handlers[options["link_right"]]));
    $(".ui-widget-overlay").click(handlers[options["out_click"]]);
    $(".ui-widget-overlay").contextmenu(handlers[options["out_right"]]);

    function resetOptions(e) {
        options = {
            "bmk_dbl": "Expand active context",
            "ws_click": "None",
            "ws_right": "Collapse item",
            "link_click": "Copy value",
            "link_right": "Expand / Special",
            "out_click": "Close dialog",
            "out_right": "Close dialog",
            "adv": "true",
            "width": "1000",
            "ctrl_list_by": "name",
            "applet_list": "applet / bc",
            "applet_list_by": "name",
            "focus_feature": "false"
        };
        localStorage.setItem(Id, JSON.stringify(options));
    }

    // render functions
    function rDropdown(caption, field, list) {
        const id = field;
        const value = options[field];
        return [`<li>`,
            `<label for="${id}">${caption}</label>`,
            `<select id="${id}">`,
            list.map((i) => `<option value="${i}" ${i == value ? 'selected' : ''}>${i}</option>`),
            `</select>`,
            `<li>`].join("");
    }

    function rOptions() {
        if ($d.find(".options").length) {
            $d.find(".options").remove();
        } else {
            let html = [
                `<div class="options">`,
                `<h4>SETTINGS</h4>`,
                rDropdown(`Advanced properties`, `adv`, [`false`, `true`]),
                rDropdown(`Dialog width`, `width`, [`600`, `800`, `1000`]),
                rDropdown(`Show in main list`, `applet_list`, [`applet`, `applet / bc`, `applet / bc / rowid`]),
                rDropdown(`List applets by`, `applet_list_by`, [`name`, `title`]),
                rDropdown(`List controls by`, `ctrl_list_by`, [`name`, `caption`]),
                rDropdown(`Link click`, `link_click`, [`Copy value`, `Copy value and close`, `Expand / Special`, `None`]),
                rDropdown(`Link right click`, `link_right`, [`Copy value`, `Copy value and close`, `Expand / Special`, `None`]),
                rDropdown(`Bookmarklet double click`, `bmk_dbl`, [`Expand active context`, `Copy active applet`]),
                rDropdown(`Whitespace click`, `ws_click`, [`None`, `Close dialog`, `Options`, `Expand active context`, `Collapse item`, `Collapse all`]),
                rDropdown(`Whitespace right click`, `ws_right`, [`None`, `Close dialog`, `Options`, `Expand active context`, `Collapse item`, `Collapse all`]),
                rDropdown(`Outside click`, `out_click`, [`Close dialog`, `Expand active context`, `Collapse item`, `Collapse all`]),
                rDropdown(`Outside right click`, `out_right`, [`Close dialog`, `Expand active context`, `Collapse item`, `Collapse all`]),
                rDropdown(`Focus feature`, `focus_feature`, [`false`, `true`]),
                `<\div>`
            ].join("\n");
            $d.append(html);
            $d.find("select").change((e) => {
                const c = e?.target;
                if (c) {
                    options[c.id] = c.value;
                    localStorage.setItem(Id, JSON.stringify(options));
                }
            });
        }
    }

    function rPS(prop) {
        return rItem(`<a href='#'>${prop[0]}</a>`, prop[1]);
    }

    function rHierarchy(caption, value, advanced) {
        var a = [];
        while ("object" === typeof value && value?.constructor?.name?.length > 1) {
            a.push(value.constructor.name);
            value = value.constructor.superclass;
        }
        return rItem(caption, a, advanced);
    }

    function rItem(caption, value, advanced, attribs = {}) {
        if (value && (!Array.isArray(value) || value.length) || "boolean" === typeof value) {
            if (!advanced || options["adv"] === `true`) {
                guid++;
                let id = Id + guid;
                let sAttr = Object.entries(attribs).map(([p, v]) => `${p}="${v}"`).join(" ");
                return (Array.isArray(value) ? [
                    `<li>`,
                    `<label for="${id}_0">`, caption, `:</label> `,
                    value.map((e, i) => `<a href="#" id="${id}_${i}" ${sAttr}>${e}</a>`).join(" > "),
                    `</li>`
                ] : [
                    `<li>`,
                    `<label for="${id}">`, caption, `:</label> `,
                    `<a href="#" id="${id}" ${sAttr}>`, escapeHtml(value), `</a>`,
                    `</li>`
                ]).join("");
            }
        }
        return "";
    }

    function rControl(control) {
        const id = control.GetInputName();
        const applet = control.GetApplet();
        const pr = SiebelAppFacade.ComponentMgr.FindComponent(applet.GetName())?.GetPR();
        const bc = applet.GetBusComp();
        const ps = control.GetMethodPropSet();
        const up = control.GetPMPropSet();
        const con = SiebelApp.Constants;
        let sel = `#${applet.GetFullId()} [name=${control.GetInputName()}]`;
        if (con.get("SWE_CTRL_RTCEMBEDDED") === control.GetUIType()) {
            sel = `#${applet.GetFullId()} #cke_${control.GetInputName()}`;
        }
        const cls = control === applet.GetActiveControl() ? 'x_active' : $(sel).length == 0 || $(sel).is(":visible") ? '' : 'x_hidden'
        return [`<li>`,
            `<a href="#" data-ul="${id}_c" class="${cls}">`,
            options["ctrl_list_by"] == 'caption' && control.GetDisplayName() ? control.GetDisplayName() : control.GetName(),
            `</a>`,
            `<ul id="${id}_c" class="ul_hide">`,
            rItem(control.GetControlType() == con.get("SWE_PST_COL") ? "List column" : control.GetControlType() == con.get("SWE_PST_CNTRL") ? "Control" : control.GetControlType(), control.GetName()),
            rItem("Display name", control.GetDisplayName()),
            rItem("Field", control.GetFieldName(), false, { "data-handler": "field", "data-selector": `ul#${applet.GetFullId()}_bc,ul#${applet.GetFullId()}_bc_${applet.GetBusComp().GetFieldMap()[control.GetFieldName()]?.index}` }),
            rItem("Value", bc?.GetFieldValue(control.GetFieldName())),
            rItem("Message", control.GetControlMsg()),
            ...(control.GetMessageVariableMap() && Object.keys(control.GetMessageVariableMap()).length ?
                [`<li><a href="#" data-ul="${id}_var">Message variables (${Object.keys(control.GetMessageVariableMap()).length}):</a></li>`,
                `<ul id="${id}_var" class="ul_hide">`,
                Object.entries(control.GetMessageVariableMap()).map(rPS).join("\n"),
                    `</ul>`] : []),
            rItem("Type", control.GetUIType()),
            rItem("LOV", control.GetLovType()),
            rItem("MVG", control.IsMultiValue()),
            rItem("Method", control.GetMethodName(), false, { "data-handler": "control method", "data-applet": applet.GetName(), "data-control": control.GetName() }),
            ...(options["adv"] === `true` && ps && ps.propArrayLen ?
                [`<li><a href="#" data-ul="${id}_method">Method properties (${ps.propArrayLen}):</a></li>`,
                `<ul id="${id}_method" class="ul_hide">`,
                Object.entries(ps.propArray).map(rPS).join("\n"),
                    `</ul>`] : []),
            //rItem("Id", id),
            rItem("Id", options["adv"] === `true` && $(sel).is(":focusable") ? [id, `<a href="#" data-focus='${sel}'>Focus</a>`] : id),
            rItem("Immidiate post changes", control.GetPostChanges()),
            rItem("Display format", control.GetDisplayFormat()),
            rItem("HTML attribute", control.GetHTMLAttr(), true),
            rItem("Display mode", control.GetDispMode()),
            rItem("Popup", control.GetPopupType() && [control.GetPopupType(), control.GetPopupWidth(), control.GetPopupHeight()].join(" / ")),
            rHierarchy("Plugin wrapper", SiebelApp.S_App.PluginBuilder.GetPwByControl(pr, control), true),
            rItem("Length", control.GetFieldName() ? control.GetMaxSize() : ""),
            ...(options["adv"] === `true` && up && up.propArrayLen ?
                [`<li><a href="#" data-ul="${id}_up">User properties (${up.propArrayLen}):</a></li>`,
                `<ul id="${id}_up" class="ul_hide">`,
                Object.entries(up.propArray).map(rPS).join("\n"),
                    `</ul>`] : []),
            rItem("Object", `SiebelApp.S_App.GetActiveView().GetAppletMap()["${applet.GetName()}"].GetControls()["${control.GetName()}"]`, true),
            $(sel).length > 0 ? rItem("Node", `$("${sel}")`, true) : ``,
            `</ul>`, `</li>`].join("");
    }

    function rApplet(applet) {
        const cm = Object.values(applet.GetControls());
        const mm = applet.GetCanInvokeArray();
        const id = applet.GetFullId();
        return [`<ul id="${id}" class="ul_hide">`,
        rItem("Applet", applet.GetName()),
        rItem("BusComp", applet.GetBusComp()?.GetName(), false, { "data-handler": "buscomp", "data-selector": `ul#${applet.GetFullId()}_bc` }),
        rItem("Title", SiebelApp.S_App.LookupStringCache(applet.GetTitle())),
        rItem("Mode", applet.GetMode()),
        rItem("Record counter", applet.GetPModel().GetStateUIMap().GetRowCounter, true),
        rHierarchy("PModel", applet.GetPModel(), true),
        rHierarchy("PRender", SiebelAppFacade.ComponentMgr.FindComponent(applet.GetName())?.GetPR(), true),
        rItem("Object", `SiebelApp.S_App.GetActiveView().GetAppletMap()["${applet.GetName()}"]`, true),
        rItem("Node", `$("#${applet.GetFullId()}")`, true),
        `<li><a href="#" data-ul="${id}_methods">Methods (${mm.length}):</a></li>`,
        `<ul id="${id}_methods" class="ul_hide">`,
        mm.map(m => [`<li>`, `<a href="#" data-handler="applet method" data-applet="${applet.GetName()}">`, m, `</a>`, `</li>`].join("")).join("\n"),
            `</ul>`,
        `<li><a href="#" data-ul="${id}_controls">Controls (${cm.length}):</a></li>`,
        `<ul id="${id}_controls" class="ul_show keep_open">`, //<ul id="${id}_controls" class="ul_show">
        ...cm.map(rControl),
            `</ul>`,
            `</ul>`].join("\n");
    }

    function rField(field, id) {
        const bc = field.GetBusComp();
        const name = SiebelApp.S_App.LookupStringCache(field.GetName());
        return [`<li>`,
            `<a href="#" data-ul="${id}">`, name, `</a>`,
            `<ul id="${id}" class="ul_hide">`,
            rItem("Field", name),
            rItem("Value", field.GetBusComp().GetFieldValue(name)),
            rItem("Type", field.GetDataType()),
            rItem("Length", field.GetLength()),
            rItem("Search spec", field.GetSearchSpec()),
            rItem("Calculated", !!field.IsCalc()),
            rItem("Bounded picklist", !!field.IsBoundedPick()),
            rItem("Read only", !!field.IsReadOnly()),
            rItem("Immediate post changes", !!field.IsPostChanges()),
            rItem("Object", `SiebelApp.S_App.GetBusObj().GetBusCompByName("${bc.GetName()}").GetFieldMap()["${name}"]`, true),
            `</ul>`, `</li>`].join("\n");
    }

    function rBC(a, id) {
        var bc = a.GetBusComp();
        const fields = Object.values(bc.GetFieldMap());
        return [`<ul id="${id}" class="ul_hide">`,
        rItem("BusComp", bc.GetName()),
        rItem("Commit pending", !!bc.commitPending, true),
        rItem("Can update", !!bc.canUpdate),
        rItem("Search spec", bc.GetSearchSpec()),
        rItem("Sort spec", bc.GetSortSpec()),
        rItem("Current row id", bc.GetIdValue()),
        rItem("Object", `SiebelApp.S_App.GetBusObj().GetBusComp("${a.GetBCId()}")`, true),
        `<li><label><a href="#" data-ul="${id}_rec">Records: ${Math.abs(bc.GetCurRowNum())} of ${bc.GetNumRows()}${bc.IsNumRowsKnown() ? '' : '+'}</a></label></li>`,
        `<ul id="${id}_rec" class="ul_hide">`,
            `<table>`,
            `<tr>`,
        ...Object.keys(bc.GetFieldMap()).map((i) => `<th>${i}</th>`),
            `</tr>`,
        ...bc.GetRecordSet().map((r, i) => [
            `<tr>`,
            ...Object.values(r).map(v => [
                `<td><a href="#" ${bc.GetSelection() == i ? ` class="x_active"` : ``}>`,
                v,
                `</a></td>`
            ].join("")),
            `</tr>`
        ].join("")),
            `</table>`,
            `</ul>`,
        `<li><label><a href="#" data-ul="${id}_fields">Fields(${bc.GetFieldList()?.length}):</a></label></li>`,
        `<ul id="${id}_fields" class="ul_show keep_open">`,
        ...fields.map((field, i) => rField(field, id + "_" + field.index)),
            `</ul>`,
            `</ul>`].join("\n");
    }

    
     
  function GetMachine() {
    var machinename = "";
    var Svc = SiebelApp.S_App.GetService("FaCS Utilities Service");
    var psInputs = SiebelApp.S_App.NewPropertySet();
    psInputs.SetProperty("Name", 'COMPUTERNAME');
    var lp = {
        'async': false,
        'cb': function (methodname, psInputs, psOutputs) {
            machinename = psOutputs.GetChildByType("ResultSet").GetProperty("Value");
        },
        'scope': this
    };
    try {
        Svc.InvokeMethod("GetEnvVar", psInputs, lp);
        return machinename;
    } catch (e) {
    }
  }
    
    function rApplication() {
        const app = SiebelApp.S_App;
        const view = app.GetActiveView();
        const bo = app?.GetBusObj();
        const bm = bo?.GetBCArray();
        const scrPM = SiebelApp.S_App.NavCtrlMngr()?.GetscreenNavigationPM();
        let am = Object.values(view?.GetAppletMap());
        var ws = SiebelApp.S_App.GetWSInfo().split("_");
        var wsver = ws.pop();

        var amCache = {};
        Object.assign(amCache, view?.GetAppletMap());

        // Identifying a primary BC
        var paa = Object.values(SiebelApp.S_App.GetActiveView().GetAppletMap()).filter((a) => !a.GetParentApplet() && (!a.GetBusComp() || !a.GetBusComp().GetParentBusComp()));
        if (!paa.length) {
            alert("Failed to identify a primary BusComp!")
        }

        return [`<ul>`,
            rItem("Machine", GetMachine()),
            rItem("Application", app.GetName()),
            rItem("Screen", scrPM?.Get("GetTabInfo")[scrPM?.Get("GetSelectedTabKey")]?.screenName),
            rItem("View", view.GetName()),
            rItem("Task", view.GetActiveTask()),
            rHierarchy("PModel", SiebelAppFacade.ComponentMgr.FindComponent(view.GetName())?.GetPM(), true),
            rHierarchy("PRender", SiebelAppFacade.ComponentMgr.FindComponent(view.GetName())?.GetPR(), true),
            rItem("BusObject", bo?.GetName()),
            rItem("Workspace", [ws.join("_"), wsver]),
            `<label>Applets (${am.length}) / BusComps (${bm.length}):</label>`,
            `<ul>`,
            hierBC(paa[0].GetBusComp(), 0, amCache),
            ...Object.values(amCache).map((a) => rAppletName(a, 0, amCache)),
            `</ul></ul>`].join("\n");
    }

    // prints applet name
    function rAppletName(a, l, amCache) {
        delete amCache[a.GetName()];
        return [`<li>`,
            `<ul>`.repeat(l),
            `<a href="#" data-ul="${a.GetFullId()}" class="${a === SiebelApp.S_App.GetActiveView().GetActiveApplet() ? 'x_active' : $(`#${a.GetFullId()}`).is(":visible") ? '' : 'x_hidden'}">`,
            options['applet_list_by'] == 'title' && SiebelApp.S_App.LookupStringCache(a.GetTitle()) ? SiebelApp.S_App.LookupStringCache(a.GetTitle()) : a.GetName(),
            `</a>`,
            a.GetBusComp() && options["applet_list"].indexOf("bc") > -1 ? ` | <a href="#" data-ul="${a.GetFullId()}_bc" class="${a === SiebelApp.S_App.GetActiveView().GetActiveApplet() ? 'x_active' : $(`#${a.GetFullId()}`).is(":visible") ? '' : 'x_hidden'}">${a.GetBusComp().GetName()}</a>` : ``,
            a.GetBusComp() && a.GetBusComp().GetIdValue() ? ` | <a href="#" class="${a === SiebelApp.S_App.GetActiveView().GetActiveApplet() ? 'x_active' : $(`#${a.GetFullId()}`).is(":visible") ? '' : 'x_hidden'}">${a.GetBusComp().GetIdValue()}</a>` : ``,
            rApplet(a),
            a.GetBusComp() && rBC(a, a.GetFullId() + "_bc"),
            `</ul>`.repeat(l),
            `</li>`].join("");
    }

    // prints applets based on bc or parent applet (rec)
    function hierApplet(bc, pa, l, amCache) {
        return Object.values(amCache).filter((a) => bc && a.GetBusComp() === bc || pa && a.GetParentApplet() === pa).map((a) => !(a.GetName() in amCache) ? "" : [
            rAppletName(a, l, amCache),
            hierApplet(null, a, l + 1, amCache) // look for child applets
        ].join("\n"));
    }

    // prints applets based on BC hierarchy (rec)
    function hierBC(bc, l, amCache) {
        return [
            hierApplet(bc, null, l, amCache)?.join("\n"),
            ...SiebelApp.S_App.GetActiveBusObj().GetBCArray().filter((e) => e.GetParentBusComp() === bc).map((b) => hierBC(b, l + 1, amCache))
        ].join("\n");
    }

    // utilities
    function escapeHtml(html) {
        return html.toString()
            .replace(/&/g, "&")
            .replace(/</g, "<")
            .replace(/>/g, ">")
            .replace(/"/g, '"')
            .replace(/'/g, "'");
    }
})() 