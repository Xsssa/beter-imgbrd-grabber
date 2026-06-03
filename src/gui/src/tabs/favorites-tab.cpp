#include "tabs/favorites-tab.h"
#include <QMenu>
#include <QMessageBox>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QFile>
#include <QSettings>
#include <QShortcut>
#include <QUrl>
#include <QtMath>
#include <ui_favorites-tab.h>
#include <algorithm>
#include "downloader/download-query-group.h"
#include "favorite-window.h"
#include "functions.h"
#include "helpers.h"
#include "logger.h"
#include "main-window.h"
#include "models/favorite.h"
#include "models/page.h"
#include "models/profile.h"
#include "models/site.h"
#include "network/network-reply.h"
#include "ui/fixed-size-grid-layout.h"
#include "ui/QAffiche.h"
#include "ui/QBouton.h"
#include "ui/text-edit.h"

#define FAVORITES_THUMB_SIZE 150
#define FAVORITES_THUMB_REPAIR_CONCURRENT_LIMIT 2
#define FAVORITES_THUMB_REPAIR_MAX_ATTEMPTS 8

namespace
{
	QPixmap favoritePlaceholder(const QString &name, int size)
	{
		QPixmap pix(size, size);
		pix.fill(Qt::transparent);

		QPainter painter(&pix);
		painter.setRenderHint(QPainter::Antialiasing);

		const QRectF rect = pix.rect().adjusted(4, 4, -4, -4);
		QPainterPath path;
		path.addRoundedRect(rect, 12, 12);

		painter.fillPath(path, QColor(10, 24, 32));
		painter.setPen(QPen(QColor(45, 224, 208, 170), 2));
		painter.drawPath(path);

		QString marker = QStringLiteral("?");
		for (const QChar &ch : name) {
			if (ch.isLetterOrNumber()) {
				marker = QString(ch).toUpper();
				break;
			}
		}

		QFont font = painter.font();
		font.setBold(true);
		font.setPointSize(qMax(18, size / 3));
		painter.setFont(font);
		painter.setPen(QColor(210, 244, 240));
		painter.drawText(pix.rect(), Qt::AlignCenter, marker);

		return pix;
	}
}


FavoritesTab::FavoritesTab(Profile *profile, DownloadQueue *downloadQueue, MainWindow *parent)
	: SearchTab(profile, downloadQueue, parent, "Favorites"), ui(new Ui::FavoritesTab), m_currentFav(0)
{
	ui->setupUi(this);

	// Promote favorites layout into fixed-size grid layout
	const int hSpace = m_settings->value("Margins/horizontal", 6).toInt();
	const int vSpace = m_settings->value("Margins/vertical", 6).toInt();
	m_favoritesLayout = new FixedSizeGridLayout(hSpace, vSpace);
	const bool fixedWidthLayout = m_settings->value("resultsFixedWidthLayout", false).toBool();
	if (fixedWidthLayout) {
		const int borderSize = m_settings->value("borders", 3).toInt();
		const qreal upscale = m_settings->value("thumbnailUpscale", 1.0).toDouble();
		m_favoritesLayout->setFixedWidth(qFloor(FAVORITES_THUMB_SIZE * upscale + borderSize * 2));
	}
	auto *layoutWidget = new QWidget;
	layoutWidget->setLayout(m_favoritesLayout);
	ui->layoutFavorites->addWidget(layoutWidget, 0, 0);

	// UI members for SearchTab class
	ui_checkMergeResults = ui->checkMergeResults;
	ui_progressMergeResults = ui->progressMergeResults;
	ui_stackedMergeResults = ui->stackedMergeResults;
	ui_spinPage = ui->spinPage;
	ui_spinImagesPerPage = ui->spinImagesPerPage;
	ui_spinColumns = ui->spinColumns;
	ui_layoutResults = ui->layoutResults;
	ui_layoutSourcesList = ui->layoutSourcesList;
	ui_buttonHistoryBack = ui->buttonHistoryBack;
	ui_buttonHistoryNext = ui->buttonHistoryNext;
	ui_buttonNextPage = ui->buttonNextPage;
	ui_buttonLastPage = ui->buttonLastPage;
	ui_buttonGetAll = ui->buttonGetAll;
	ui_buttonGetPage = ui->buttonGetpage;
	ui_buttonGetSel = ui->buttonGetSel;
	ui_buttonFirstPage = ui->buttonFirstPage;
	ui_buttonPreviousPage = ui->buttonPreviousPage;
	ui_scrollAreaResults = ui->scrollAreaResults;

	// Search field
	m_postFiltering = createAutocomplete();
	ui->layoutPlus->addWidget(m_postFiltering, 1, 1, 1, 3);

	// Open tab with closed result splitter
	ui->splitter->setSizes({ 1, 0 });

	// Others
	ui->checkMergeResults->setChecked(m_settings->value("mergeresults", false).toBool());
	optionsChanged();
	ui->widgetPlus->hide();
	setWindowIcon(QIcon());
	updateCheckboxes();

	static const QStringList assoc { QStringLiteral("name"), QStringLiteral("note"), QStringLiteral("lastviewed") };
		ui->comboOrder->setCurrentIndex(assoc.indexOf(m_settings->value("Favorites/order", "name").toString()));
		ui->comboAsc->setCurrentIndex(static_cast<int>(m_settings->value("Favorites/reverse", false).toBool()));
		m_settings->setValue("reverse", ui->comboAsc->currentIndex() == 1);
	ui->widgetResults->hide();

	auto *closeShortcut = new QShortcut(getKeySequence(m_settings, "Main/Shortcuts/keyFavoritesBack", Qt::Key_Escape), this);
	closeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
	connect(closeShortcut, &QShortcut::activated, this, &FavoritesTab::favoritesBack);

	connect(m_profile, &Profile::favoritesChanged, this, &FavoritesTab::updateFavorites);
	updateFavorites();
}

FavoritesTab::~FavoritesTab()
{
	close();
	delete ui;
}

void FavoritesTab::closeEvent(QCloseEvent *event)
{
	m_settings->setValue("mergeresults", ui->checkMergeResults->isChecked());
	m_settings->beginGroup("Favorites");
		static const QStringList assoc { QStringLiteral("name"), QStringLiteral("note"), QStringLiteral("lastviewed") };
		m_settings->setValue("order", assoc[ui->comboOrder->currentIndex()]);
		m_settings->setValue("reverse", ui->comboAsc->currentIndex() == 1);
	m_settings->endGroup();
	m_settings->sync();

	emit closed(this);
	event->accept();
}



void FavoritesTab::updateFavorites()
{
	static const QStringList assoc { "name", "note", "lastviewed" };
	const QString &order = assoc[ui->comboOrder->currentIndex()];
	const bool reverse = ui->comboAsc->currentIndex() == 1;

	QList<Favorite> favorites = m_favorites;

	if (order == "note") {
		std::sort(favorites.begin(), favorites.end(), Favorite::sortByNote);
	} else if (order == "lastviewed") {
		std::sort(favorites.begin(), favorites.end(), Favorite::sortByLastViewed);
	} else {
		std::sort(favorites.begin(), favorites.end(), Favorite::sortByName);
	}
	if (reverse) {
		favorites = reversed(favorites);
	}

	clearLayout(m_favoritesLayout);

	if (m_favorites.isEmpty()) {
		ui->labelFavorites->setText(tr("You don't have any favorite yet."));
		ui->labelFavorites->show();
	} else {
		ui->labelFavorites->hide();
	}

	QString display = m_settings->value("favorites_display", "ind").toString();
	const qreal upscale = m_settings->value("thumbnailUpscale", 1.0).toDouble();
	const int borderSize = m_settings->value("borders", 3).toInt();
	const int imageSize = qFloor(FAVORITES_THUMB_SIZE * upscale);
	const int dim = imageSize + borderSize * 2;

	for (Favorite &fav : favorites) {
		const QString xt = tr("<b>Name:</b> %1<br/><b>Note:</b> %2 %<br/><b>Last view:</b> %3").arg(fav.getName(), QString::number(fav.getNote()), QLocale().toString(fav.getLastViewed(), QLocale::ShortFormat));
		auto *w = new QWidget(ui->scrollAreaWidgetContents);
		auto *l = new QVBoxLayout;
		l->setContentsMargins(0, 0, 0, 0);
		w->setLayout(l);

		int maxNewImages = 0;
		bool precise = true;
		for (const Monitor &monitor : qAsConst(fav.getMonitors())) {
			if (monitor.cumulated() > maxNewImages) {
				maxNewImages = monitor.cumulated();
				precise = monitor.preciseCumulated();
			}
		}

		if (display.contains("i")) {
			const bool resizeInsteadOfCropping = m_settings->value("resizeInsteadOfCropping", true).toBool();

			QPixmap img = fav.getImage();
			if (img.isNull()) {
				img = favoritePlaceholder(fav.getName(), imageSize);
			}
			if (!hasSavedThumbnail(fav)) {
				scheduleFavoriteThumbnailRepair(fav);
			}

			auto *image = new QBouton(fav.getName(), resizeInsteadOfCropping, false, 0, QColor(), this);
				image->scale(img, QSize(imageSize, imageSize));
				image->setFixedSize(dim, dim);
				image->setFlat(true);
				image->setToolTip(xt);
				connect(image, SIGNAL(rightClick(QString)), this, SLOT(favoriteProperties(QString)));
				connect(image, SIGNAL(middleClick(QString)), m_parent, SLOT(addTab(QString)));
				connect(image, SIGNAL(appui(QString)), this, SLOT(loadFavorite(QString)));

			if (maxNewImages > 0)
			{ image->setCounter(QString::number(maxNewImages) + (!precise ? "+" : QString())); }

			l->addWidget(image);
		}

		QString label;
		if (display.contains("n")) {
			label += fav.getName();
			if (maxNewImages > 0 && !display.contains("i")) {
				label += QStringLiteral(" <b style='color:red'>(%1%2)</b>").arg(maxNewImages).arg(!precise ? "+" : QString());
			}
		}
		if (display.contains("d")) {
			label += "<br/>(" + QString::number(fav.getNote()) + " % - " + QLocale().toString(fav.getLastViewed().date(), QLocale::ShortFormat) + ")";
		}

		auto *caption = new QAffiche(fav.getName(), 0, QColor(), this);
			caption->setText(label);
			caption->setTextFormat(Qt::RichText);
			caption->setAlignment(Qt::AlignCenter);
			caption->setToolTip(xt);
			caption->setFixedWidth(dim);
		if (!caption->text().isEmpty()) {
			connect(caption, SIGNAL(rightClicked(QString)), this, SLOT(favoriteProperties(QString)));
			connect(caption, SIGNAL(middleClicked(QString)), m_parent, SLOT(addTab(QString)));
			connect(caption, SIGNAL(clicked(QString)), this, SLOT(loadFavorite(QString)));
			l->addWidget(caption);
		}

		m_favoritesLayout->addWidget(w);
	}
}

bool FavoritesTab::hasSavedThumbnail(const Favorite &favorite) const
{
	const QString path = favorite.getImagePath();
	return !path.isEmpty()
		&& !path.startsWith(QStringLiteral(":/"))
		&& QFile::exists(path)
		&& !favorite.getImage().isNull();
}

void FavoritesTab::scheduleFavoriteThumbnailRepair(const Favorite &favorite)
{
	const QString tag = favorite.getName();
	if (tag.isEmpty() || m_thumbnailRepairAttempted.contains(tag)) {
		return;
	}
	if (m_thumbnailRepairAttempted.size() >= FAVORITES_THUMB_REPAIR_MAX_ATTEMPTS) {
		return;
	}

	const int activeRepairs = m_thumbnailRepairPages.size() + m_thumbnailRepairReplies.size();
	if (activeRepairs >= FAVORITES_THUMB_REPAIR_CONCURRENT_LIMIT) {
		return;
	}

	QList<Site*> sites = favorite.getSites();
	if (sites.isEmpty()) {
		sites = m_selectedSources;
	}
	if (sites.isEmpty()) {
		return;
	}

	m_thumbnailRepairAttempted.insert(tag);

	Site *site = sites.first();
	QStringList search = tag.trimmed().split(' ', Qt::SkipEmptyParts);
	search.append(m_settings->value("add").toString().trimmed().split(' ', Qt::SkipEmptyParts));
	auto *page = new Page(m_profile, site, m_sites.values(), SearchQuery(search), 1, 1, favorite.getPostFiltering(), false, this);
	m_thumbnailRepairPages.insert(page, tag);
	connect(page, &Page::finishedLoading, this, &FavoritesTab::repairFavoriteThumbnailPageLoaded);
	connect(page, &Page::failedLoading, this, [this, page]() {
		const QString tag = m_thumbnailRepairPages.take(page);
		finishFavoriteThumbnailRepair(tag);
		page->deleteLater();
	});
	if (!page->isValid()) {
		const QString tag = m_thumbnailRepairPages.take(page);
		finishFavoriteThumbnailRepair(tag);
		page->deleteLater();
		return;
	}
	page->load(true);
}

void FavoritesTab::repairFavoriteThumbnailPageLoaded(Page *page)
{
	const QString tag = m_thumbnailRepairPages.take(page);
	if (tag.isEmpty()) {
		page->deleteLater();
		return;
	}

	if (page->images().isEmpty()) {
		finishFavoriteThumbnailRepair(tag);
		page->deleteLater();
		return;
	}

	QSharedPointer<Image> image;
	for (const QSharedPointer<Image> &img : page->images()) {
		if (img->isValid()) {
			image = img;
			break;
		}
	}
	if (image.isNull()) {
		finishFavoriteThumbnailRepair(tag);
		page->deleteLater();
		return;
	}

	const qreal upscale = m_settings->value("thumbnailUpscale", 1.0).toDouble();
	const int imageSize = qFloor(FAVORITES_THUMB_SIZE * upscale);
	const QUrl thumbnailUrl = m_settings->value("thumbnailSmartSize", true).toBool()
		? image->mediaForSize(QSize(imageSize, imageSize), true).url
		: image->url(Image::Size::Thumbnail);

	if (!thumbnailUrl.isValid()) {
		finishFavoriteThumbnailRepair(tag);
		page->deleteLater();
		return;
	}

	Site *site = image->parentSite();
	NetworkReply *reply = site->get(site->fixUrl(thumbnailUrl.toString()), Site::QueryType::Thumbnail, image->parentUrl(), "favorite-preview");
	m_thumbnailRepairReplies.insert(reply, tag);
	m_thumbnailRepairReplySites.insert(reply, site);
	connect(reply, &NetworkReply::finished, this, &FavoritesTab::repairFavoriteThumbnailReplyFinished);
	page->deleteLater();
}

void FavoritesTab::repairFavoriteThumbnailReplyFinished()
{
	auto *reply = qobject_cast<NetworkReply*>(sender());
	if (reply == nullptr) {
		return;
	}

	const QString tag = m_thumbnailRepairReplies.take(reply);
	Site *site = m_thumbnailRepairReplySites.take(reply);
	if (tag.isEmpty()) {
		reply->deleteLater();
		return;
	}

	const QUrl redirection = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
	if (!redirection.isEmpty() && site != nullptr) {
		NetworkReply *retry = site->get(site->fixUrl(redirection.toString()), Site::QueryType::Thumbnail, {}, "favorite-preview");
		m_thumbnailRepairReplies.insert(retry, tag);
		m_thumbnailRepairReplySites.insert(retry, site);
		connect(retry, &NetworkReply::finished, this, &FavoritesTab::repairFavoriteThumbnailReplyFinished);
		reply->deleteLater();
		return;
	}

	QPixmap thumbnail;
	if (reply->error() == NetworkReply::NetworkError::NoError) {
		thumbnail.loadFromData(reply->readAll());
	} else if (site != nullptr) {
		const QString ext = getExtension(reply->url());
		if (!ext.isEmpty() && ext != "jpg") {
			NetworkReply *retry = site->get(site->fixUrl(setExtension(reply->url(), "jpg").toString()), Site::QueryType::Thumbnail, {}, "favorite-preview");
			m_thumbnailRepairReplies.insert(retry, tag);
			m_thumbnailRepairReplySites.insert(retry, site);
			connect(retry, &NetworkReply::finished, this, &FavoritesTab::repairFavoriteThumbnailReplyFinished);
			reply->deleteLater();
			return;
		}
	}
	reply->deleteLater();

	if (thumbnail.isNull()) {
		finishFavoriteThumbnailRepair(tag);
		return;
	}

	const int index = m_favorites.indexOf(Favorite(tag));
	if (index >= 0 && m_favorites[index].setImage(thumbnail)) {
		m_profile->emitFavorite();
	} else {
		finishFavoriteThumbnailRepair(tag);
	}
}

void FavoritesTab::finishFavoriteThumbnailRepair(const QString &tag)
{
	Q_UNUSED(tag)
	if (m_thumbnailRepairPages.size() + m_thumbnailRepairReplies.size() < FAVORITES_THUMB_REPAIR_CONCURRENT_LIMIT) {
		updateFavorites();
	}
}


void FavoritesTab::load()
{
	updateTitle();
	m_newLastImages.clear();

	loadTags(m_currentTags.trimmed().split(' ', Qt::SkipEmptyParts));
}

bool FavoritesTab::validateImage(const QSharedPointer<Image> &img, QString &error)
{
	bool dateOk = img->createdAt().isNull() || img->createdAt() > m_loadFavorite;

	const QVariantMap lastIdentity = m_lastImages.value(img->parentSite()->url());
	bool idOk = !lastIdentity.contains("id") || img->id() > lastIdentity["id"].toULongLong();

	if (dateOk && idOk && !m_newLastImages.contains(img->parentSite()->url())) {
		m_newLastImages[img->parentSite()->url()] = img->identity(true);
	}

	return dateOk && idOk && SearchTab::validateImage(img, error);
}

void FavoritesTab::write(QJsonObject &json) const
{
	Q_UNUSED(json)
}


void FavoritesTab::addResultsPage(Page *page, const QList<QSharedPointer<Image>> &imgs, bool merged, int filteredImages, const QString &noResultsMessage)
{
	Q_UNUSED(noResultsMessage)

	SearchTab::addResultsPage(page, imgs, merged, filteredImages, tr("No result since the %1").arg(QLocale().toString(m_loadFavorite, QLocale::ShortFormat)));
	ui->splitter->setSizes({ (m_images.count() >= m_settings->value("hidefavorites", 20).toInt() ? 0 : 1), 1 });
}

void FavoritesTab::setPageLabelText(QLabel *txt, Page *page, const QList<QSharedPointer<Image>> &imgs, int filteredImages, const QString &noResultsMessage)
{
	Q_UNUSED(noResultsMessage)

	SearchTab::setPageLabelText(txt, page, imgs, filteredImages, tr("No result since the %1").arg(QLocale().toString(m_loadFavorite, QLocale::ShortFormat)));
}

void FavoritesTab::setTags(const QString &tags, bool preload)
{
	m_currentTags = tags;

	if (preload) {
		activateWindow();
		load();
	} else {
		updateTitle();
	}
}

void FavoritesTab::getPage()
{
	const bool unloaded = m_settings->value("getunloadedpages", false).toBool();

	QList<QSharedPointer<Page>> pages = this->getPagesToDownload();
	for (const QSharedPointer<Page> &page : pages) {
		const QStringList search = (m_currentTags + " " + m_settings->value("add").toString().toLower().trimmed()).split(' ', Qt::SkipEmptyParts);
		const int perpage = unloaded ? ui->spinImagesPerPage->value() : page->pageImageCount();
		const QStringList postFiltering = postFilter(true);

		emit batchAddGroup(DownloadQueryGroup(m_settings, search, ui->spinPage->value(), perpage, perpage, postFiltering, page->site()));
	}
}
void FavoritesTab::getAll()
{
	QList<QSharedPointer<Page>> pages = this->getPagesToDownload();
	for (const QSharedPointer<Page> &page : pages) {
		const int highLimit = page->highLimit();
		const int currentCount = page->pageImageCount();
		const int imageCount = page->imagesCount() >= 0 ? page->imagesCount() : page->maxImagesCount();
		const int total = imageCount > 0 ? qMax(currentCount, imageCount) : -1;
		const int perPage = highLimit > 0 ? (imageCount > 0 ? qMin(highLimit, imageCount) : highLimit) : currentCount;
		if ((perPage == 0 && total == 0) || (currentCount == 0 && imageCount <= 0)) {
			continue;
		}

		const QStringList search = (m_currentTags + " " + m_settings->value("add").toString().toLower().trimmed()).split(' ', Qt::SkipEmptyParts);
		const QStringList postFiltering = postFilter(true);

		emit batchAddGroup(DownloadQueryGroup(m_settings, search, 1, perPage, total, postFiltering, page->site()));
	}
}


QList<Site*> FavoritesTab::sources()
{ return m_selectedSources; }

QString FavoritesTab::tags() const
{ return m_currentTags; }

void FavoritesTab::loadFavorite(const QString &name)
{
	const int index = name.isEmpty() ? m_currentFav : m_favorites.indexOf(Favorite(name));
	if (index < 0) {
		return;
	}

	Favorite fav = m_favorites[index];
	m_currentTags = fav.getName();
	m_loadFavorite = fav.getLastViewed();
	m_lastImages = fav.getLastImages();
	m_postFiltering->setPlainText(fav.getPostFiltering().join(' '));

	if (!fav.getSites().isEmpty()) {
		setSources(fav.getSites());
	}

	ui->widgetResults->show();
	load();
}
void FavoritesTab::checkFavorites()
{
	ui->widgetFavorites->show();
	m_currentFav = -1;
	m_currentTags = QString();
	loadNextFavorite();
}
void FavoritesTab::loadNextFavorite()
{
	if (m_currentFav + 1 >= m_favorites.count()) {
		return;
	}

	m_currentFav++;
	m_currentTags = m_favorites[m_currentFav].getName();
	m_loadFavorite = m_favorites[m_currentFav].getLastViewed();

	load();
}
void FavoritesTab::viewed()
{
	if (m_currentTags.isEmpty()) {
		const int reponse = QMessageBox::question(this, tr("Mark as viewed"), tr("Are you sure you want to mark all your favorites as viewed?"), QMessageBox::Yes | QMessageBox::No);
		if (reponse == QMessageBox::Yes) {
			for (const Favorite &fav : qAsConst(m_favorites)) {
				setFavoriteViewed(fav.getName());
			}
		}
	} else {
		setFavoriteViewed(m_currentTags);
	}

	m_profile->emitFavorite();
}
void FavoritesTab::setFavoriteViewed(const QString &tag, const QSharedPointer<Image> &img)
{
	log(QStringLiteral("Marking \"%1\" as viewed...").arg(tag));

	const int index = tag.isEmpty() ? m_currentFav : m_favorites.indexOf(Favorite(tag));
	if (index < 0) {
		return;
	}

	Favorite &fav = m_favorites[index];

	if (img.isNull()) {
		fav.setLastViewed(QDateTime::currentDateTime());

		for (Monitor &monitor : fav.getMonitors()) {
			monitor.setCumulated(0, true);
		}

		for (auto it = m_newLastImages.constBegin(); it != m_newLastImages.constEnd(); ++it) {
			fav.setLastImage(it.key(), it.value());
		}
	} else {
		if (img->createdAt().isValid()) {
			fav.setLastViewed(img->createdAt());
		}
		fav.setLastImage(img->parentSite()->url(), img->identity(true));
	}

	DONE();
}
void FavoritesTab::favoritesBack()
{
	ui->widgetResults->hide();
	ui->widgetFavorites->show();
	ui->splitter->setSizes({ 1, 0 });

	if (!m_currentTags.isEmpty() || m_currentFav != -1) {
		m_currentTags = QString();
		m_currentFav = -1;
		ui->widgetFavorites->show();
	}
}
void FavoritesTab::favoriteProperties(const QString &name)
{
	const int index = name.isEmpty() ? m_currentFav : m_favorites.indexOf(Favorite(name));
	if (index < 0) {
		return;
	}

	const Favorite fav = m_favorites[index];
	auto *favoriteWindow = new FavoriteWindow(m_profile, fav, this);
	favoriteWindow->show();
}

void FavoritesTab::focusSearch()
{}


void FavoritesTab::changeEvent(QEvent *event)
{
	// Automatically re-translate this tab on language change
	if (event->type() == QEvent::LanguageChange) {
		ui->retranslateUi(this);
		updateTitle();
	}

	QWidget::changeEvent(event);
}

void FavoritesTab::thumbnailContextMenu(QMenu *menu, const QSharedPointer<Image> &img)
{
	SearchTab::thumbnailContextMenu(menu, img);

	if (m_currentTags.isEmpty()) {
		return;
	}

	QAction *first = menu->actions().first();

	// Mark as "last viewed"
	auto *actionMarkAsLastViewed = new QAction(QIcon(":/images/icons/eye.png"), tr("Mark as last viewed"), menu);
	connect(actionMarkAsLastViewed, &QAction::triggered, [this, img]() {
		this->setFavoriteViewed(m_currentTags, img);
	});
	menu->insertAction(first, actionMarkAsLastViewed);

	// Choose selected image as favorite thumbnail
	auto *actionUseAsThumbnail = new QAction(QIcon(":/images/icons/save.png"), tr("Choose as image"), menu);
	connect(actionUseAsThumbnail, &QAction::triggered, [this, img]() {
		const int index = m_favorites.indexOf(Favorite(m_currentTags));
		if (index >= 0) {
			m_favorites[index].setImage(img->previewImage());
		}
		m_profile->emitFavorite();
	});
	menu->insertAction(first, actionUseAsThumbnail);

	menu->insertSeparator(first);
}

void FavoritesTab::thumbnailLoaded(const QSharedPointer<Image> &img)
{
	if (m_currentTags.isEmpty() || img->previewImage().isNull()) {
		return;
	}

	const int index = m_favorites.indexOf(Favorite(m_currentTags));
	if (index < 0 || hasSavedThumbnail(m_favorites[index])) {
		return;
	}

	if (m_favorites[index].setImage(img->previewImage())) {
		log(QStringLiteral("Repaired favorite thumbnail for \"%1\" from loaded results.").arg(m_currentTags), Logger::Info);
		m_profile->emitFavorite();
	}
}

void FavoritesTab::updateTitle()
{
	setWindowTitle(tr("Favorites") + (m_currentTags.isEmpty() ? "" : " - " + m_currentTags));
	emit titleChanged(this);
}

void FavoritesTab::splitterMoved(int pos, int index)
{
	const QString title = tr("Favorites");

	int min, max;
	ui->splitter->getRange(index, &min, &max);

	if (index == 1 && pos >= max) {
		setWindowTitle(title);
		emit titleChanged(this);
	} else if (windowTitle() == title) {
		updateTitle();
	}
}
