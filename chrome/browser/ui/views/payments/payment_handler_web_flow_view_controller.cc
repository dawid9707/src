// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/payments/payment_handler_web_flow_view_controller.h"

#include <memory>

#include "base/base64.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/payments/ssl_validity_checker.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/views/payments/payment_request_dialog_view.h"
#include "chrome/browser/ui/views/payments/payment_request_views_util.h"
#include "chrome/grit/generated_resources.h"
#include "components/payments/content/origin_security_checker.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/gfx/color_palette.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/progress_bar.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/layout/grid_layout.h"

namespace payments {

class ReadOnlyOriginView : public views::View {
 public:
  ReadOnlyOriginView(const base::string16& page_title,
                     const GURL& origin,
                     const gfx::ImageSkia* icon_image_skia,
                     SkColor background_color,
                     views::ButtonListener* site_settings_listener) {
    std::unique_ptr<views::View> title_origin_container =
        std::make_unique<views::View>();
    SkColor foreground = GetForegroundColorForBackground(background_color);
    views::GridLayout* title_origin_layout =
        title_origin_container->SetLayoutManager(
            std::make_unique<views::GridLayout>(title_origin_container.get()));

    views::ColumnSet* columns = title_origin_layout->AddColumnSet(0);
    columns->AddColumn(views::GridLayout::LEADING, views::GridLayout::FILL, 1,
                       views::GridLayout::USE_PREF, 0, 0);

    bool title_is_valid = !page_title.empty();
    if (title_is_valid) {
      title_origin_layout->StartRow(0, 0);
      std::unique_ptr<views::Label> title_label =
          std::make_unique<views::Label>(page_title,
                                         views::style::CONTEXT_DIALOG_TITLE);
      title_label->set_id(static_cast<int>(DialogViewID::SHEET_TITLE));
      title_label->SetFocusBehavior(
          views::View::FocusBehavior::ACCESSIBLE_ONLY);
      // Turn off autoreadability because the computed |foreground| color takes
      // contrast into account.
      title_label->SetAutoColorReadabilityEnabled(false);
      title_label->SetEnabledColor(foreground);
      title_origin_layout->AddView(title_label.release());
    }

    title_origin_layout->StartRow(0, 0);
    views::Label* origin_label =
        new views::Label(base::UTF8ToUTF16(origin.host()));
    origin_label->SetElideBehavior(gfx::ELIDE_HEAD);
    if (!title_is_valid) {
      // Set the origin as title when the page title is invalid.
      origin_label->set_id(static_cast<int>(DialogViewID::SHEET_TITLE));

      // Pad to keep header as the same height as when the page title is valid.
      constexpr int kVerticalPadding = 10;
      origin_label->SetBorder(
          views::CreateEmptyBorder(kVerticalPadding, 0, kVerticalPadding, 0));
    }
    // Turn off autoreadability because the computed |foreground| color takes
    // contrast into account.
    origin_label->SetAutoColorReadabilityEnabled(false);
    origin_label->SetEnabledColor(foreground);

    origin_label->SetBackgroundColor(background_color);
    title_origin_layout->AddView(origin_label);

    views::GridLayout* top_level_layout =
        SetLayoutManager(std::make_unique<views::GridLayout>(this));
    views::ColumnSet* top_level_columns = top_level_layout->AddColumnSet(0);
    top_level_columns->AddColumn(views::GridLayout::LEADING,
                                 views::GridLayout::CENTER, 1,
                                 views::GridLayout::USE_PREF, 0, 0);
    // Payment handler icon comes from Web Manifest, which are square.
    constexpr int kPaymentHandlerIconSize = 32;
    bool has_icon = icon_image_skia && icon_image_skia->width();
    if (has_icon) {
      // A column for the instrument icon.
      top_level_columns->AddColumn(
          views::GridLayout::LEADING, views::GridLayout::FILL, 0,
          views::GridLayout::FIXED, kPaymentHandlerIconSize,
          kPaymentHandlerIconSize);
      top_level_columns->AddPaddingColumn(0, 8);
    }

    top_level_layout->StartRow(0, 0);
    top_level_layout->AddView(title_origin_container.release());
    if (has_icon) {
      std::unique_ptr<views::ImageView> instrument_icon_view =
          CreateInstrumentIconView(/*icon_id=*/0, icon_image_skia,
                                   /*label=*/page_title);
      instrument_icon_view->SetImageSize(
          gfx::Size(kPaymentHandlerIconSize, kPaymentHandlerIconSize));
      top_level_layout->AddView(instrument_icon_view.release());
    }
  }
  ~ReadOnlyOriginView() override {}

 private:
  DISALLOW_COPY_AND_ASSIGN(ReadOnlyOriginView);
};

PaymentHandlerWebFlowViewController::PaymentHandlerWebFlowViewController(
    PaymentRequestSpec* spec,
    PaymentRequestState* state,
    PaymentRequestDialogView* dialog,
    Profile* profile,
    GURL target,
    PaymentHandlerOpenWindowCallback first_navigation_complete_callback)
    : PaymentRequestSheetController(spec, state, dialog),
      profile_(profile),
      target_(target),
      show_progress_bar_(false),
      progress_bar_(
          std::make_unique<views::ProgressBar>(/*preferred_height=*/2)),
      separator_(std::make_unique<views::Separator>()),
      first_navigation_complete_callback_(
          std::move(first_navigation_complete_callback)) {
  progress_bar_->set_owned_by_client();
  progress_bar_->set_foreground_color(gfx::kGoogleBlue500);
  progress_bar_->set_background_color(SK_ColorTRANSPARENT);
  separator_->set_owned_by_client();
  separator_->SetColor(separator_->GetNativeTheme()->GetSystemColor(
      ui::NativeTheme::kColorId_SeparatorColor));
}

PaymentHandlerWebFlowViewController::~PaymentHandlerWebFlowViewController() {}

base::string16 PaymentHandlerWebFlowViewController::GetSheetTitle() {
  if (web_contents())
    return web_contents()->GetTitle();

  return l10n_util::GetStringUTF16(IDS_TAB_LOADING_TITLE);
}

void PaymentHandlerWebFlowViewController::FillContentView(
    views::View* content_view) {
  content_view->SetLayoutManager(std::make_unique<views::FillLayout>());
  std::unique_ptr<views::WebView> web_view =
      std::make_unique<views::WebView>(profile_);
  Observe(web_view->GetWebContents());
  web_contents()->SetDelegate(this);
  web_view->LoadInitialURL(target_);

  // The webview must get an explicitly set height otherwise the layout doesn't
  // make it fill its container. This is likely because it has no content at the
  // time of first layout (nothing has loaded yet). Because of this, set it to.
  // total_dialog_height - header_height. On the other hand, the width will be
  // properly set so it can be 0 here.
  web_view->SetPreferredSize(gfx::Size(0, kDialogHeight - 75));
  content_view->AddChildView(web_view.release());
}

bool PaymentHandlerWebFlowViewController::ShouldShowSecondaryButton() {
  return false;
}

std::unique_ptr<views::View>
PaymentHandlerWebFlowViewController::CreateHeaderContentView() {
  const GURL origin = web_contents()
                          ? web_contents()->GetVisibleURL().GetOrigin()
                          : target_.GetOrigin();
  std::unique_ptr<views::Background> background = GetHeaderBackground();
  return std::make_unique<ReadOnlyOriginView>(
      web_contents() == nullptr ? base::string16() : web_contents()->GetTitle(),
      origin, state()->selected_instrument()->icon_image_skia(),
      background->get_color(), this);
}

views::View*
PaymentHandlerWebFlowViewController::CreateHeaderContentSeparatorView() {
  if (show_progress_bar_)
    return progress_bar_.get();
  return separator_.get();
}

std::unique_ptr<views::Background>
PaymentHandlerWebFlowViewController::GetHeaderBackground() {
  if (!web_contents())
    return PaymentRequestSheetController::GetHeaderBackground();
  return views::CreateSolidBackground(web_contents()->GetThemeColor());
}

bool PaymentHandlerWebFlowViewController::GetSheetId(DialogViewID* sheet_id) {
  *sheet_id = DialogViewID::PAYMENT_APP_OPENED_WINDOW_SHEET;
  return true;
}

bool PaymentHandlerWebFlowViewController::
    DisplayDynamicBorderForHiddenContents() {
  return false;
}

void PaymentHandlerWebFlowViewController::LoadProgressChanged(
    content::WebContents* source,
    double progress) {
  DCHECK(source == web_contents());

  progress_bar_->SetValue(progress);

  if (progress == 1.0 && show_progress_bar_) {
    show_progress_bar_ = false;
    UpdateHeaderContentSeparatorView();
    return;
  }

  if (progress < 1.0 && !show_progress_bar_) {
    show_progress_bar_ = true;
    UpdateHeaderContentSeparatorView();
    return;
  }
}

void PaymentHandlerWebFlowViewController::VisibleSecurityStateChanged(
    content::WebContents* source) {
  DCHECK(source == web_contents());
  // IsSslCertificateValid checks security_state::SecurityInfo.security_level
  // which reflects security state.
  if (!SslValidityChecker::IsSslCertificateValid(source)) {
    AbortPayment();
  }
}

void PaymentHandlerWebFlowViewController::DidStartNavigation(
    content::NavigationHandle* navigation_handle) {
  if (navigation_handle->IsSameDocument())
    return;

  UpdateHeaderView();
}

void PaymentHandlerWebFlowViewController::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  if (navigation_handle->IsSameDocument())
    return;

  if (!OriginSecurityChecker::IsOriginSecure(navigation_handle->GetURL()) ||
      (!OriginSecurityChecker::IsSchemeCryptographic(
           navigation_handle->GetURL()) &&
       !OriginSecurityChecker::IsOriginLocalhostOrFile(
           navigation_handle->GetURL()) &&
       !navigation_handle->GetURL().IsAboutBlank()) ||
      !SslValidityChecker::IsSslCertificateValid(
          navigation_handle->GetWebContents())) {
    AbortPayment();
    return;
  }

  if (first_navigation_complete_callback_) {
    std::move(first_navigation_complete_callback_)
        .Run(true, web_contents()->GetMainFrame()->GetProcess()->GetID(),
             web_contents()->GetMainFrame()->GetRoutingID());
    first_navigation_complete_callback_ = PaymentHandlerOpenWindowCallback();
  }

  UpdateHeaderView();
}

void PaymentHandlerWebFlowViewController::TitleWasSet(
    content::NavigationEntry* entry) {
  UpdateHeaderView();
}

void PaymentHandlerWebFlowViewController::DidAttachInterstitialPage() {
  AbortPayment();
}

void PaymentHandlerWebFlowViewController::AbortPayment() {
  if (web_contents())
    web_contents()->Close();

  dialog()->ShowErrorMessage();
}

}  // namespace payments
