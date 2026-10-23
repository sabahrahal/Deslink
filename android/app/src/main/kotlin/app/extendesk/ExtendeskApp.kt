package app.extendesk

import android.app.Application

class ExtendeskApp : Application() {
    override fun onCreate() {
        super.onCreate()
        Strings.init(this)
    }
}
