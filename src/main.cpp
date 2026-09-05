// Native GTK3 GUI for the PDF <-> Word converter engine (convert.h).
// Direction is auto-detected from the picked file's extension.
#include "convert.h"
#include "process_util.h"
#include <gtk/gtk.h>
#include <string>
#include <algorithm>
#include <cctype>

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string swapExtension(const std::string& path, const std::string& newExt) {
    size_t slash = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return path + newExt;
    }
    return path.substr(0, dot) + newExt;
}

struct AppWidgets {
    GtkWidget* window;
    GtkWidget* pathEntry;
    GtkWidget* infoLabel;
    GtkWidget* convertBtn;
    GtkWidget* openBtn;
    GtkWidget* statusLabel;
    std::string selectedPath;
    std::string outputPath;
    bool isWordToPdf = false;
};

void setStatus(AppWidgets* app, const std::string& text, bool isError) {
    gtk_label_set_text(GTK_LABEL(app->statusLabel), text.c_str());
    GtkStyleContext* ctx = gtk_widget_get_style_context(app->statusLabel);
    if (isError) {
        gtk_style_context_add_class(ctx, "error-label");
        gtk_style_context_remove_class(ctx, "ok-label");
    } else {
        gtk_style_context_add_class(ctx, "ok-label");
        gtk_style_context_remove_class(ctx, "error-label");
    }
}

void onFileChosen(AppWidgets* app, const std::string& path) {
    std::string lp = lower(path);
    if (endsWith(lp, ".docx")) {
        app->isWordToPdf = true;
        app->outputPath = swapExtension(path, ".pdf");
    } else if (endsWith(lp, ".pdf")) {
        app->isWordToPdf = false;
        app->outputPath = swapExtension(path, ".docx");
    } else {
        setStatus(app, "Csak .docx vagy .pdf fajlt valassz.", true);
        gtk_widget_set_sensitive(app->convertBtn, FALSE);
        return;
    }
    app->selectedPath = path;
    gtk_entry_set_text(GTK_ENTRY(app->pathEntry), path.c_str());

    std::string dir = app->isWordToPdf ? "Word -> PDF" : "PDF -> Word";
    std::string info = "Irany: " + dir + "\nKimenet: " + app->outputPath;
    gtk_label_set_text(GTK_LABEL(app->infoLabel), info.c_str());

    gtk_widget_set_sensitive(app->convertBtn, TRUE);
    gtk_widget_hide(app->openBtn);
    setStatus(app, "", false);
}

void onBrowseClicked(GtkButton*, gpointer userData) {
    auto* app = static_cast<AppWidgets*>(userData);
    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        "Fajl kivalasztasa", GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Megse", GTK_RESPONSE_CANCEL, "_Megnyitas", GTK_RESPONSE_ACCEPT, nullptr);

    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Word (.docx) vagy PDF (.pdf)");
    gtk_file_filter_add_pattern(filter, "*.docx");
    gtk_file_filter_add_pattern(filter, "*.pdf");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        onFileChosen(app, filename);
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

void onOpenClicked(GtkButton*, gpointer userData) {
    auto* app = static_cast<AppWidgets*>(userData);
    runCommand({"xdg-open", app->outputPath});
}

void onConvertClicked(GtkButton*, gpointer userData) {
    auto* app = static_cast<AppWidgets*>(userData);
    if (app->selectedPath.empty()) return;

    gtk_widget_set_sensitive(app->convertBtn, FALSE);
    setStatus(app, "Atalakitas folyamatban...", false);
    while (gtk_events_pending()) gtk_main_iteration(); // repaint before the (brief) blocking work

    try {
        if (app->isWordToPdf) convertWordToPdf(app->selectedPath, app->outputPath);
        else convertPdfToWord(app->selectedPath, app->outputPath);
        setStatus(app, "Kesz -> " + app->outputPath, false);
        gtk_widget_show(app->openBtn);
    } catch (const std::exception& e) {
        std::string what = e.what();
        if (what == "NO_TEXT_EXTRACTED") {
            setStatus(app,
                "Nem sikerult szoveget kinyerni ebbol a PDF-bol - valoszinuleg "
                "szkennelt/kep alapu PDF, ilyenekbol ez az eszkoz (meg) nem tud "
                "Word dokumentumot keszíteni.",
                true);
        } else {
            setStatus(app, "Hiba: " + what, true);
        }
        gtk_widget_hide(app->openBtn);
    }
    gtk_widget_set_sensitive(app->convertBtn, TRUE);
}

void activate(GtkApplication* gtkApp, gpointer) {
    auto* app = new AppWidgets();

    app->window = gtk_application_window_new(gtkApp);
    gtk_window_set_title(GTK_WINDOW(app->window), "PDF <-> Word atalakito");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 520, 260);
    gtk_container_set_border_width(GTK_CONTAINER(app->window), 18);

    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        ".error-label { color: #b0473a; } .ok-label { color: #3b2f26; }", -1, nullptr);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_add(GTK_CONTAINER(app->window), box);

    GtkWidget* title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='large' weight='bold'>PDF &#8596; Word atalakito</span>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

    GtkWidget* lede = gtk_label_new(
        "Valassz egy .docx vagy .pdf fajlt - az irany automatikusan a kiterjesztesbol dol el.");
    gtk_label_set_xalign(GTK_LABEL(lede), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(lede), TRUE);
    gtk_box_pack_start(GTK_BOX(box), lede, FALSE, FALSE, 0);

    GtkWidget* pickRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->pathEntry = gtk_entry_new();
    gtk_editable_set_editable(GTK_EDITABLE(app->pathEntry), FALSE);
    gtk_widget_set_hexpand(app->pathEntry, TRUE);
    GtkWidget* browseBtn = gtk_button_new_with_label("Tallozas...");
    gtk_box_pack_start(GTK_BOX(pickRow), app->pathEntry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(pickRow), browseBtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), pickRow, FALSE, FALSE, 0);

    app->infoLabel = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->infoLabel), 0.0);
    gtk_box_pack_start(GTK_BOX(box), app->infoLabel, FALSE, FALSE, 0);

    GtkWidget* actionRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->convertBtn = gtk_button_new_with_label("Atalakitas");
    gtk_widget_set_sensitive(app->convertBtn, FALSE);
    app->openBtn = gtk_button_new_with_label("Eredmeny megnyitasa");
    gtk_box_pack_start(GTK_BOX(actionRow), app->convertBtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actionRow), app->openBtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), actionRow, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(app->openBtn, TRUE); // hidden until first success

    app->statusLabel = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->statusLabel), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(app->statusLabel), TRUE);
    gtk_box_pack_start(GTK_BOX(box), app->statusLabel, FALSE, FALSE, 0);

    g_signal_connect(browseBtn, "clicked", G_CALLBACK(onBrowseClicked), app);
    g_signal_connect(app->convertBtn, "clicked", G_CALLBACK(onConvertClicked), app);
    g_signal_connect(app->openBtn, "clicked", G_CALLBACK(onOpenClicked), app);
    g_signal_connect_swapped(app->window, "destroy", G_CALLBACK(+[](gpointer p) {
        delete static_cast<AppWidgets*>(p);
    }), app);

    gtk_widget_show_all(app->window);
    gtk_widget_hide(app->openBtn);
}

} // namespace

int main(int argc, char** argv) {
    GtkApplication* gtkApp = gtk_application_new("hu.pistapp.pdfwordconverter", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtkApp, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(gtkApp), argc, argv);
    g_object_unref(gtkApp);
    return status;
}
