#pragma once


/**
 * @brief Utility for downloading images from URLs to temporary files.
 *
 * All methods are static; the class is a namespace-like grouping only.
 */
class ImageDownloader {
public:
    ImageDownloader() = delete;

    /**
     * @brief Download a URL into a uniquely named temp file.
     *
     * The file is named with a random suffix via mkstemp() and the correct
     * image extension appended so the decoder can detect the format.
     *
     * @param url  URL to download.
     * @return     Path to the temp file on success, or an empty path on failure.
     *             The CALLER is responsible for deleting the file.
     */
    static std::filesystem::path download_to_temp(const std::string &url);

    /**
     * @brief Extract a human-readable title from a URL (the filename part).
     *
     * Prefers explicit query parameters (filename=, download_filename=, *filename).
     * Falls back to the path's filename component, or "online_image".
     *
     * @param url  Full URL string.
     * @return     Filename-like title string.
     */
    static std::string title_from_url(const std::string &url);
};
